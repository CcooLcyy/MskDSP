#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <ranges>
#include <string>
#include <vector>

#include "IEC61850ConfigValidation.h"

namespace {

IEC61850Proto::PersistedConfig MakeValidConfig() {
  IEC61850Proto::PersistedConfig config;
  config.set_schema_version(1);
  auto* model = config.add_models();
  model->set_model_name("station-model");
  model->set_source_name("station.scd");
  model->set_document_kind(IEC61850Proto::SCL_DOCUMENT_KIND_SCD);
  auto* modelIed = model->add_ieds();
  modelIed->set_name("IED1");
  auto* accessPoint = modelIed->add_access_points();
  accessPoint->set_name("AP1");
  accessPoint->set_has_server(true);
  auto* attribute = modelIed->add_data_attributes();
  attribute->set_access_point("AP1");
  attribute->set_data_ref("IED1LD0/MMXU1.TotW.mag.f");
  attribute->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  attribute->set_basic_type("FLOAT32");

  auto* persistedIed = config.add_ieds();
  persistedIed->set_conn_id(10);
  auto* ied = persistedIed->mutable_config();
  ied->set_conn_name("line-1");
  ied->set_model_name("station-model");
  ied->set_ied_name("IED1");
  ied->set_access_point("AP1");
  ied->set_enable_mms(true);
  auto* channel = ied->add_channels();
  channel->set_channel(IEC61850Proto::NETWORK_CHANNEL_A);
  channel->set_enabled(true);
  channel->set_interface_name("eth0");
  channel->set_remote_ip("192.168.10.20");
  channel->set_remote_port(102);

  auto* mappings = config.add_point_mappings();
  mappings->set_conn_name("line-1");
  auto* point = mappings->add_points();
  point->set_tag("P_TOTAL");
  point->set_data_ref("IED1LD0/MMXU1.TotW.mag.f");
  point->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  point->set_source(IEC61850Proto::POINT_SOURCE_MMS);
  point->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_DOUBLE);
  point->set_scale(1.0);
  return config;
}

// 验证：完整聚合配置能够通过保存前校验。
TEST(IEC61850ConfigValidationTest, AcceptsValidAggregateConfig) {
  const auto config = MakeValidConfig();
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(issues.empty());
}

// 验证：MMS队列、批量和合批窗口等于配置上限时仍允许保存。
TEST(IEC61850ConfigValidationTest, AcceptsMmsPublishLimitsAtMaximum) {
  auto config = MakeValidConfig();
  auto* ied = config.mutable_ieds(0)->mutable_config();
  ied->set_mms_event_queue_capacity(65536);
  ied->set_publish_batch_size(4096);
  ied->set_publish_batch_window_ms(1000);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(issues.empty());
}

// 验证IED实时线程策略的普通调度默认值和显式CPU亲和性可以保存。
TEST(IEC61850ConfigValidationTest, AcceptsRealtimeThreadPolicy) {
  auto config = MakeValidConfig();
  auto* ied = config.mutable_ieds(0)->mutable_config();
  ied->add_realtime_cpu_indices(0);
  ied->set_realtime_failure_mode(
      IEC61850Proto::THREAD_RUNTIME_FAILURE_MODE_DEGRADE);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(issues.empty());
}

// 验证实时策略的非法优先级在保存前被拒绝。
TEST(IEC61850ConfigValidationTest, RejectsRealtimeThreadPriority) {
  auto config = MakeValidConfig();
  auto* ied = config.mutable_ieds(0)->mutable_config();
  ied->set_realtime_scheduling(
      IEC61850Proto::THREAD_SCHEDULING_POLICY_FIFO);
  ied->set_realtime_priority(0);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_REALTIME_POLICY_INVALID");
}

// 验证SV额定频率允许兼容默认值以及50Hz、60Hz工程配置。
TEST(IEC61850ConfigValidationTest, AcceptsSupportedSvNominalFrequencies) {
  for (const double frequency : {0.0, 50.0, 60.0}) {
    auto config = MakeValidConfig();
    config.mutable_ieds(0)->mutable_config()->set_nominal_frequency_hz(
        frequency);
    std::vector<IEC61850Proto::ValidationIssue> issues;

    const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

    EXPECT_TRUE(status.ok()) << frequency << ": " << status.error_message();
    EXPECT_TRUE(issues.empty());
  }
}

// 验证SV额定频率拒绝非标准值和非有限浮点值，避免数学窗口语义不确定。
TEST(IEC61850ConfigValidationTest, RejectsUnsupportedSvNominalFrequency) {
  for (const double frequency :
       {55.0, std::numeric_limits<double>::quiet_NaN()}) {
    auto config = MakeValidConfig();
    config.mutable_ieds(0)->mutable_config()->set_nominal_frequency_hz(
        frequency);
    std::vector<IEC61850Proto::ValidationIssue> issues;

    const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    ASSERT_FALSE(issues.empty());
    EXPECT_EQ(issues.front().code(),
              "CONFIG_SV_NOMINAL_FREQUENCY_INVALID");
  }
}

// 验证：MMS待处理点值容量超过上限时拒绝配置。
TEST(IEC61850ConfigValidationTest, RejectsMmsEventQueueCapacityAboveMaximum) {
  auto config = MakeValidConfig();
  config.mutable_ieds(0)->mutable_config()->set_mms_event_queue_capacity(65537);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(),
            "CONFIG_MMS_EVENT_QUEUE_CAPACITY_EXCEEDED");
}

// 验证：DataCenter单批点数超过上限时拒绝配置。
TEST(IEC61850ConfigValidationTest, RejectsPublishBatchSizeAboveMaximum) {
  auto config = MakeValidConfig();
  config.mutable_ieds(0)->mutable_config()->set_publish_batch_size(4097);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_PUBLISH_BATCH_SIZE_EXCEEDED");
}

// 验证：DataCenter合批窗口超过上限时拒绝配置。
TEST(IEC61850ConfigValidationTest, RejectsPublishBatchWindowAboveMaximum) {
  auto config = MakeValidConfig();
  config.mutable_ieds(0)->mutable_config()->set_publish_batch_window_ms(1001);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_PUBLISH_BATCH_WINDOW_EXCEEDED");
}

// 验证：保护规则必须绑定GOOSE、输入条件和成对的动作/释放值。
TEST(IEC61850ConfigValidationTest, ValidatesProtectionRuleShape) {
  auto config = MakeValidConfig();
  auto* rule = config.mutable_ieds(0)->mutable_config()->add_protection_rules();
  rule->set_rule_id("trip-rule");
  auto* condition = rule->add_conditions();
  condition->set_data_ref("IED1LD0/MMXU1.TotW.mag.f");
  condition->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  condition->set_comparator(IEC61850Proto::PROTECTION_COMPARATOR_EQUAL);
  condition->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_DOUBLE);
  condition->set_double_value(1.0);
  rule->set_output_control_ref("IED1LD0/LLN0$GO$gcb1");
  auto* assertValue = rule->add_assert_values();
  assertValue->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_DOUBLE);
  auto* releaseValue = rule->add_release_values();
  releaseValue->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_DOUBLE);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  auto status = IEC61850::ValidatePersistedConfig(config, &issues);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_PROTECTION_REQUIRES_GOOSE");

  config.mutable_ieds(0)->mutable_config()->set_enable_goose(true);
  issues.clear();
  status = IEC61850::ValidatePersistedConfig(config, &issues);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(issues.empty());
}

// 验证：保护输入比较器与值类型不匹配时拒绝配置。
TEST(IEC61850ConfigValidationTest, RejectsProtectionComparatorTypeMismatch) {
  auto config = MakeValidConfig();
  auto* ied = config.mutable_ieds(0)->mutable_config();
  ied->set_enable_goose(true);
  auto* rule = ied->add_protection_rules();
  rule->set_rule_id("invalid-rule");
  auto* condition = rule->add_conditions();
  condition->set_data_ref("IED1LD0/MMXU1.TotW.mag.f");
  condition->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  condition->set_comparator(
      IEC61850Proto::PROTECTION_COMPARATOR_GREATER_THAN);
  condition->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->set_output_control_ref("IED1LD0/LLN0$GO$gcb1");
  rule->add_assert_values()->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->add_release_values()->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
    return issue.code() == "CONFIG_PROTECTION_NUMERIC_TYPE_MISMATCH";
  }));
}

// 验证：重复模型名和重复IED连接名会在保存前被拒绝。
TEST(IEC61850ConfigValidationTest, RejectsDuplicateStableNames) {
  auto config = MakeValidConfig();
  *config.add_models() = config.models(0);
  *config.add_ieds() = config.ieds(0);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_GE(issues.size(), 2u);
  EXPECT_EQ(issues[0].severity(), IEC61850Proto::VALIDATION_SEVERITY_ERROR);
}

// 验证：IED引用不存在的模型或模型中不存在的IED时被拒绝。
TEST(IEC61850ConfigValidationTest, RejectsDanglingIedModelReferences) {
  auto config = MakeValidConfig();
  config.mutable_ieds(0)->mutable_config()->set_model_name("missing-model");
  std::vector<IEC61850Proto::ValidationIssue> issues;

  auto status = IEC61850::ValidatePersistedConfig(config, &issues);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_MODEL_NOT_FOUND");

  config = MakeValidConfig();
  config.mutable_ieds(0)->mutable_config()->set_ied_name("MISSING_IED");
  issues.clear();
  status = IEC61850::ValidatePersistedConfig(config, &issues);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_IED_NOT_FOUND_IN_MODEL");
}

// 验证：IED配置的AccessPoint必须存在于所选模型IED中。
TEST(IEC61850ConfigValidationTest, RejectsMissingAccessPoint) {
  auto config = MakeValidConfig();
  config.mutable_ieds(0)->mutable_config()->set_access_point("MISSING_AP");
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_ACCESS_POINT_NOT_FOUND");
}

// 验证：模型AccessPoint名称为空时拒绝聚合配置。
TEST(IEC61850ConfigValidationTest, RejectsEmptyAccessPointName) {
  auto config = MakeValidConfig();
  config.mutable_models(0)->mutable_ieds(0)->mutable_access_points(0)->clear_name();
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_ACCESS_POINT_NAME_EMPTY");
}

// 验证：启用协议功能的目标AccessPoint必须直接包含Server模型。
TEST(IEC61850ConfigValidationTest, RejectsAccessPointWithoutServer) {
  auto config = MakeValidConfig();
  config.mutable_models(0)
      ->mutable_ieds(0)
      ->mutable_access_points(0)
      ->set_has_server(false);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_TRUE(std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
    return issue.code() == "CONFIG_ACCESS_POINT_SERVER_MISSING";
  }));
}

// 验证：A/B通道编号唯一且启用的MMS通道必须提供网卡、远端IP和端口。
TEST(IEC61850ConfigValidationTest, ValidatesNetworkChannels) {
  auto config = MakeValidConfig();
  *config.mutable_ieds(0)->mutable_config()->add_channels() =
      config.ieds(0).config().channels(0);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  auto status = IEC61850::ValidatePersistedConfig(config, &issues);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_DUPLICATE_CHANNEL");

  config = MakeValidConfig();
  config.mutable_ieds(0)->mutable_config()->mutable_channels(0)->clear_remote_ip();
  issues.clear();
  status = IEC61850::ValidatePersistedConfig(config, &issues);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_MMS_ENDPOINT_INCOMPLETE");
}

// 验证：点映射的tag和data_ref+fc组合在一个IED内都必须唯一。
TEST(IEC61850ConfigValidationTest, RejectsDuplicatePointMappings) {
  auto config = MakeValidConfig();
  *config.mutable_point_mappings(0)->add_points() = config.point_mappings(0).points(0);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_DUPLICATE_POINT_TAG");
}

// 验证：点映射必须指定FC并引用所选模型IED中存在的数据属性。
TEST(IEC61850ConfigValidationTest, ValidatesPointDataReferenceAndFc) {
  auto config = MakeValidConfig();
  config.mutable_point_mappings(0)->mutable_points(0)->set_fc(
      IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  auto status = IEC61850::ValidatePersistedConfig(config, &issues);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_POINT_FC_UNSPECIFIED");

  config = MakeValidConfig();
  config.mutable_point_mappings(0)->mutable_points(0)->set_data_ref(
      "IED1LD0/MMXU1.Missing.mag.f");
  issues.clear();
  status = IEC61850::ValidatePersistedConfig(config, &issues);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_POINT_DATA_REF_NOT_FOUND");
}

// 验证：GOOSE点映射通过当前AccessPoint的ExtRef源引用校验，不要求本地DA目录存在同名属性。
TEST(IEC61850ConfigValidationTest, ValidatesGooseMappingAgainstExternalReference) {
  auto config = MakeValidConfig();
  auto* ied = config.mutable_ieds(0)->mutable_config();
  ied->set_enable_mms(false);
  ied->set_enable_goose(true);
  config.mutable_models(0)->mutable_ieds(0)->clear_data_attributes();
  auto* extRef = config.mutable_models(0)->mutable_ieds(0)->add_ext_refs();
  extRef->set_access_point("AP1");
  extRef->set_source_data_ref("PublisherLD0/PTRC1.Tr.general");
  extRef->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  extRef->set_service_type("GOOSE");
  config.mutable_point_mappings(0)->mutable_points(0)->set_data_ref(
      "PublisherLD0/PTRC1.Tr.general");
  config.mutable_point_mappings(0)->mutable_points(0)->set_fc(
      IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  config.mutable_point_mappings(0)->mutable_points(0)->set_source(
      IEC61850Proto::POINT_SOURCE_GOOSE);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  EXPECT_TRUE(IEC61850::ValidatePersistedConfig(config, &issues).ok());
  EXPECT_TRUE(issues.empty());
}

// 验证：SV派生点是内部计算结果，缺少原始SCL数据属性时仍可通过配置校验。
TEST(IEC61850ConfigValidationTest, AllowsSvDerivedPointWithoutRawAttribute) {
  auto config = MakeValidConfig();
  auto* ied = config.mutable_ieds(0)->mutable_config();
  ied->set_enable_mms(false);
  ied->set_enable_sv(true);
  config.mutable_models(0)->mutable_ieds(0)->clear_data_attributes();
  auto* point = config.mutable_point_mappings(0)->mutable_points(0);
  point->set_tag("RMS");
  point->set_data_ref("SV_DERIVED/IED1/RMS");
  point->set_source(IEC61850Proto::POINT_SOURCE_SV_DERIVED);
  point->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_DOUBLE);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  EXPECT_TRUE(IEC61850::ValidatePersistedConfig(config, &issues).ok());
  EXPECT_TRUE(issues.empty());
}

// 验证：另一个AccessPoint中的同名data_ref和FC不能满足当前AP的点映射。
TEST(IEC61850ConfigValidationTest, RejectsPointReferenceFromDifferentAccessPoint) {
  auto config = MakeValidConfig();
  auto* modelIed = config.mutable_models(0)->mutable_ieds(0);
  auto* accessPoint = modelIed->add_access_points();
  accessPoint->set_name("AP2");
  accessPoint->set_has_server(true);
  modelIed->mutable_data_attributes(0)->set_access_point("AP2");
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.back().code(), "CONFIG_POINT_DATA_REF_NOT_FOUND");
}

// 验证：旧模型只有一个Server AP时，缺失AP归属的对象可安全兼容。
TEST(IEC61850ConfigValidationTest, AcceptsLegacyUnscopedObjectForSingleServerAccessPoint) {
  auto config = MakeValidConfig();
  config.mutable_models(0)
      ->mutable_ieds(0)
      ->mutable_data_attributes(0)
      ->clear_access_point();
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(issues.empty());
}

// 验证：旧模型存在多个Server AP时，缺失AP归属的对象必须重新导入而不能猜测。
TEST(IEC61850ConfigValidationTest, RejectsLegacyUnscopedObjectForMultipleServerAccessPoints) {
  auto config = MakeValidConfig();
  auto* modelIed = config.mutable_models(0)->mutable_ieds(0);
  auto* accessPoint = modelIed->add_access_points();
  accessPoint->set_name("AP2");
  accessPoint->set_has_server(true);
  modelIed->mutable_data_attributes(0)->clear_access_point();
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_ACCESS_POINT_MODEL_UNSCOPED");
}

// 验证：模型对象显式归属不存在或未包含Server的AP时都会被拒绝。
TEST(IEC61850ConfigValidationTest, RejectsInvalidExplicitAccessPointOwnership) {
  auto config = MakeValidConfig();
  config.mutable_models(0)
      ->mutable_ieds(0)
      ->mutable_data_attributes(0)
      ->set_access_point("MISSING_AP");
  std::vector<IEC61850Proto::ValidationIssue> issues;

  auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_ACCESS_POINT_MODEL_INVALID");

  config = MakeValidConfig();
  auto* modelIed = config.mutable_models(0)->mutable_ieds(0);
  auto* clientAccessPoint = modelIed->add_access_points();
  clientAccessPoint->set_name("CLIENT_AP");
  clientAccessPoint->set_has_server(false);
  modelIed->mutable_data_attributes(0)->set_access_point("CLIENT_AP");
  issues.clear();

  status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_ACCESS_POINT_MODEL_INVALID");
}

// 验证：Report、GSE和SV控制块不能引用另一个AP中的同名DataSet。
TEST(IEC61850ConfigValidationTest, RejectsControlDataSetReferenceAcrossAccessPoints) {
  auto config = MakeValidConfig();
  auto* modelIed = config.mutable_models(0)->mutable_ieds(0);
  auto* secondAccessPoint = modelIed->add_access_points();
  secondAccessPoint->set_name("AP2");
  secondAccessPoint->set_has_server(true);
  auto* dataSet = modelIed->add_data_sets();
  dataSet->set_access_point("AP1");
  dataSet->set_data_set_ref("IED1LD0/LLN0$events");
  auto* report = modelIed->add_report_controls();
  report->set_access_point("AP2");
  report->set_data_set_ref(dataSet->data_set_ref());
  auto* gse = modelIed->add_gse_controls();
  gse->set_access_point("AP2");
  gse->set_data_set_ref(dataSet->data_set_ref());
  auto* sampledValue = modelIed->add_sampled_value_controls();
  sampledValue->set_access_point("AP2");
  sampledValue->set_data_set_ref(dataSet->data_set_ref());
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(std::count_if(issues.begin(), issues.end(), [](const auto& issue) {
              return issue.code() == "CONFIG_CONTROL_DATASET_NOT_FOUND";
            }),
            3);
}

// 验证启用GI的ReportControl必须同时启用ReasonCode，避免普通报告误判为GI。
TEST(IEC61850ConfigValidationTest, RejectsGeneralInterrogationWithoutReasonCode) {
  auto config = MakeValidConfig();
  auto* modelIed = config.mutable_models(0)->mutable_ieds(0);
  auto* dataSet = modelIed->add_data_sets();
  dataSet->set_access_point("AP1");
  dataSet->set_data_set_ref("IED1LD0/LLN0$events");
  auto* member = dataSet->add_members();
  member->set_data_ref("IED1LD0/MMXU1.TotW.mag.f");
  member->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  auto* report = modelIed->add_report_controls();
  report->set_access_point("AP1");
  report->set_rcb_ref("IED1LD0/LLN0$BR$brcb1");
  report->set_data_set_ref(dataSet->data_set_ref());
  report->mutable_trigger_options()->set_general_interrogation(true);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(std::ranges::any_of(issues, [](const auto& issue) {
    return issue.code() == "CONFIG_REPORT_GI_REQUIRES_REASON_CODE";
  }));
}

// 验证：点来源对应的协议功能必须在IED配置中启用。
TEST(IEC61850ConfigValidationTest, RejectsPointSourceForDisabledProtocol) {
  auto config = MakeValidConfig();
  config.mutable_ieds(0)->mutable_config()->set_enable_mms(false);
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_POINT_SOURCE_DISABLED");
}

// 验证：点映射只能引用已配置的IED连接名。
TEST(IEC61850ConfigValidationTest, RejectsDanglingPointMappingTable) {
  auto config = MakeValidConfig();
  config.mutable_point_mappings(0)->set_conn_name("missing-line");
  std::vector<IEC61850Proto::ValidationIssue> issues;

  const auto status = IEC61850::ValidatePersistedConfig(config, &issues);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  ASSERT_FALSE(issues.empty());
  EXPECT_EQ(issues.front().code(), "CONFIG_MAPPING_IED_NOT_FOUND");
}

}  // namespace
