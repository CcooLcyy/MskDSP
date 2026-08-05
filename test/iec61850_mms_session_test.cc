#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "IEC61850MmsChannelPolicy.h"
#include "IEC61850MmsSession.h"
#include "mskdsp/IEC61850Limits.hpp"

namespace {

IEC61850::ProtocolIedPlan MakePlan() {
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("line-1");
  plan.config.set_ied_name("IED1");
  plan.config.set_access_point("AP1");
  plan.ied.set_name("IED1");
  auto* node = plan.ied.add_logical_nodes();
  node->set_node_ref("IED1LD0/LLN0");
  auto* attribute = plan.ied.add_data_attributes();
  attribute->set_data_ref("IED1LD0/MMXU1.TotW.mag.f");
  attribute->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  auto* dataSet = plan.ied.add_data_sets();
  dataSet->set_data_set_ref("IED1LD0/LLN0$measurements");
  auto* member = dataSet->add_members();
  member->set_data_ref(attribute->data_ref());
  member->set_fc(attribute->fc());
  auto* report = plan.ied.add_report_controls();
  report->set_rcb_ref("IED1LD0/LLN0$BR$brcb1");
  report->set_data_set_ref(dataSet->data_set_ref());
  report->set_buffered(true);
  report->set_config_revision(7);
  report->set_max_instances(2);
  report->set_integrity_period_ms(5000);
  report->set_buffer_time_ms(20);
  report->set_report_id("IED1/Measurements");
  report->mutable_trigger_options()->set_data_change(true);
  report->mutable_trigger_options()->set_general_interrogation(true);
  report->mutable_optional_fields()->set_sequence_number(true);
  report->mutable_optional_fields()->set_config_revision(true);
  report->mutable_optional_fields()->set_reason_code(true);
  return plan;
}

IEC61850::MmsOnlineDirectory MakeDirectory() {
  IEC61850::MmsOnlineDirectory directory;
  directory.iedName = "IED1";
  directory.accessPoint = "AP1";
  directory.logicalNodeRefs.emplace_back("IED1LD0/LLN0");
  directory.dataAttributes.push_back(
      {"IED1LD0/MMXU1.TotW.mag.f",
       IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX});
  IEC61850::MmsDirectoryDataSet dataSet;
  dataSet.dataSetRef = "IED1LD0/LLN0$measurements";
  dataSet.members = directory.dataAttributes;
  directory.dataSets.emplace_back(std::move(dataSet));
  IEC61850::MmsDirectoryReportControl report;
  report.rcbRef = "IED1LD0/LLN0$BR$brcb1";
  report.dataSetRef = "IED1LD0/LLN0$measurements";
  report.reportId = "IED1/Measurements";
  report.buffered = true;
  report.configRevision = 7;
  report.maxInstances = 2;
  report.integrityPeriodMs = 5000;
  report.bufferTimeMs = 20;
  report.triggerOptions.set_data_change(true);
  report.triggerOptions.set_general_interrogation(true);
  report.optionalFields.set_sequence_number(true);
  report.optionalFields.set_config_revision(true);
  report.optionalFields.set_reason_code(true);
  directory.reportControls.emplace_back(std::move(report));
  return directory;
}

IEC61850::MmsReportEvent MakeReport(bool generalInterrogation) {
  IEC61850::MmsReportEvent report;
  report.reportRef = "IED1LD0/LLN0$BR$brcb1";
  report.dataSetRef = "IED1LD0/LLN0$measurements";
  report.confRev = 7;
  report.generalInterrogation = generalInterrogation;
  report.values.push_back(
      {"IED1LD0/MMXU1.TotW.mag.f", IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX,
       std::int64_t{1}});
  return report;
}

// 验证：在线目录包含启动计划中的对象时允许进入RCB核对阶段，并生成完整启用请求。
TEST(IEC61850MmsSessionTest, AcceptsExpectedDirectoryAndBuildsRcbRequests) {
  const auto plan = MakePlan();
  IEC61850::MmsSessionContract contract(plan);
  auto directory = MakeDirectory();
  directory.logicalNodeRefs.emplace_back("IED1LD0/LLN0$Extra");

  ASSERT_TRUE(contract.ValidateOnlineDirectory(directory).ok());
  const auto requests = contract.BuildRcbActivationRequests();

  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests.front().rcbRef, "IED1LD0/LLN0$BR$brcb1");
  EXPECT_EQ(requests.front().dataSetRef, "IED1LD0/LLN0$measurements");
  EXPECT_TRUE(requests.front().buffered);
  EXPECT_EQ(requests.front().configRevision, 7u);
  EXPECT_EQ(requests.front().maxInstances, 2u);
  EXPECT_TRUE(requests.front().generalInterrogation);
}

// 验证：在线目录缺少配置对象时不得被额外对象或其他引用满足。
TEST(IEC61850MmsSessionTest, RejectsMissingDirectoryObject) {
  const auto plan = MakePlan();
  IEC61850::MmsSessionContract contract(plan);
  auto directory = MakeDirectory();
  directory.dataAttributes.clear();

  const auto status = contract.ValidateOnlineDirectory(directory);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("数据属性"), std::string::npos);
}

// 验证：在线目录必须来自启动计划指定的IED和AccessPoint，不能用其他AP满足引用。
TEST(IEC61850MmsSessionTest, RejectsDirectoryFromOtherAccessPoint) {
  const auto plan = MakePlan();
  IEC61850::MmsSessionContract contract(plan);
  auto directory = MakeDirectory();
  directory.accessPoint = "AP2";

  const auto status = contract.ValidateOnlineDirectory(directory);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("AccessPoint"), std::string::npos);
}

// 验证：DataSet成员数量或顺序变化时拒绝在线目录，避免错误映射数据。
TEST(IEC61850MmsSessionTest, RejectsMismatchedDataSetMembers) {
  const auto plan = MakePlan();
  IEC61850::MmsSessionContract contract(plan);
  auto directory = MakeDirectory();
  directory.dataSets.front().members.front().dataRef = "IED1LD0/MMXU1.Other";

  const auto status = contract.ValidateOnlineDirectory(directory);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("DataSet成员"), std::string::npos);
}

// 验证DATASETS阶段在没有RCB在线属性时仍会严格核对成员顺序。
TEST(IEC61850MmsSessionTest, DatasetValidationStageChecksOnlineMembers) {
  const auto plan = MakePlan();
  IEC61850::MmsSessionContract contract(plan);
  auto directory = MakeDirectory();
  directory.reportControls.clear();

  ASSERT_TRUE(contract.ValidateOnlineDirectory(
                  directory, IEC61850::MmsDirectoryValidationStage::DATASETS)
                  .ok());
  directory.dataSets.front().members.front().dataRef =
      "IED1LD0/MMXU1.Other";
  EXPECT_EQ(contract.ValidateOnlineDirectory(
                directory, IEC61850::MmsDirectoryValidationStage::DATASETS)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：在线RCB的ConfRev或参数变化时禁止静默启用。
TEST(IEC61850MmsSessionTest, RejectsMismatchedRcbConfigurationRevision) {
  const auto plan = MakePlan();
  IEC61850::MmsSessionContract contract(plan);
  auto directory = MakeDirectory();
  directory.reportControls.front().configRevision = 8;

  const auto status = contract.ValidateOnlineDirectory(directory);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("ConfRev"), std::string::npos);
}

// 验证：在线ReportControl的ReportID变化时不得静默启用错误的报告实例。
TEST(IEC61850MmsSessionTest, RejectsMismatchedReportId) {
  const auto plan = MakePlan();
  IEC61850::MmsSessionContract contract(plan);
  auto directory = MakeDirectory();
  directory.reportControls.front().reportId = "IED1/Other";

  const auto status = contract.ValidateOnlineDirectory(directory);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("ReportControl参数"),
            std::string::npos);
}

// 验证：在线目录中的重复RCB引用会被拒绝，避免按首个对象静默运行。
TEST(IEC61850MmsSessionTest, RejectsDuplicateOnlineReportControl) {
  const auto plan = MakePlan();
  IEC61850::MmsSessionContract contract(plan);
  auto directory = MakeDirectory();
  directory.reportControls.push_back(directory.reportControls.front());

  const auto status = contract.ValidateOnlineDirectory(directory);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("ReportControl引用重复"),
            std::string::npos);
}

// 验证：规范化SCL引用能按Domain和MMS美元号路径编码为Domain对象名。
TEST(IEC61850MmsSessionTest, ConvertsSclReferenceToMmsDomainObjectName) {
  IEC61850::MmsObjectName objectName;
  ASSERT_TRUE(IEC61850::ParseMmsDomainObjectReference(
                  "IED1LD0/MMXU1.TotW.mag.f", &objectName)
                  .ok());

  EXPECT_EQ(objectName.type, IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC);
  EXPECT_EQ(objectName.domain, "IED1LD0");
  EXPECT_EQ(objectName.identifier, "MMXU1$TotW$mag$f");

  ASSERT_TRUE(IEC61850::ParseMmsDomainObjectReference(
                  "IED1LD0/LLN0$measurements", &objectName)
                  .ok());
  EXPECT_EQ(objectName.identifier, "LLN0$measurements");
}

// 验证：没有唯一Domain/Item分隔符的引用不能进入MMS对象名编码。
TEST(IEC61850MmsSessionTest, RejectsInvalidMmsDomainObjectReference) {
  IEC61850::MmsObjectName objectName;
  EXPECT_EQ(IEC61850::ParseMmsDomainObjectReference("IED1LD0", &objectName)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(IEC61850::ParseMmsDomainObjectReference("/MMXU1$TotW", &objectName)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：基础目录阶段可以先核对变量和DataSet，不能因尚未实现RCB读取而伪造完整就绪。
TEST(IEC61850MmsSessionTest, BaseDirectoryValidationDoesNotRequireRcb) {
  const auto plan = MakePlan();
  IEC61850::MmsSessionContract contract(plan);
  auto directory = MakeDirectory();
  directory.reportControls.clear();

  ASSERT_TRUE(contract.ValidateOnlineDirectory(
                  directory, IEC61850::MmsDirectoryValidationStage::BASE)
                  .ok());
  EXPECT_FALSE(contract.Ready());
}

// 验证：在线TypeSpecification与SCL基础类型一致时允许核对，不一致时拒绝。
TEST(IEC61850MmsSessionTest, ValidatesOnlineTypeSpecificationAgainstScl) {
  auto plan = MakePlan();
  plan.ied.mutable_data_attributes(0)->set_basic_type("INT32");
  IEC61850::MmsSessionContract contract(plan);
  auto directory = MakeDirectory();
  IEC61850::MmsTypeSpecification type;
  type.kind = IEC61850::MmsTypeSpecificationKind::INTEGER;
  type.width = 32;
  directory.dataAttributes.front().typeSpecification = type;

  ASSERT_TRUE(contract.ValidateOnlineDirectory(directory).ok());

  auto mismatch = MakeDirectory();
  IEC61850::MmsTypeSpecification wrong;
  wrong.kind = IEC61850::MmsTypeSpecificationKind::BOOLEAN;
  mismatch.dataAttributes.front().typeSpecification = wrong;
  EXPECT_EQ(IEC61850::MmsSessionContract(plan)
                .ValidateOnlineDirectory(mismatch)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：RCB启用确认和GI完成确认齐全后才允许会话报告Ready。
TEST(IEC61850MmsSessionTest, RequiresRcbEnableAndGeneralInterrogation) {
  const auto plan = MakePlan();
  IEC61850::MmsSessionContract contract(plan);

  EXPECT_FALSE(contract.Ready());
  ASSERT_TRUE(contract.ValidateOnlineDirectory(MakeDirectory()).ok());
  EXPECT_EQ(contract.MarkRcbEnabled("IED1LD0/LLN0$BR$brcb1", 6).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  ASSERT_TRUE(
      contract.MarkRcbEnabled("IED1LD0/LLN0$BR$brcb1", 7).ok());
  EXPECT_FALSE(contract.Ready());
  auto giReport = MakeReport(true);
  EXPECT_EQ(contract.MarkGeneralInterrogationComplete(giReport).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  ASSERT_TRUE(contract.MarkGeneralInterrogationRequested(
                  "IED1LD0/LLN0$BR$brcb1")
                  .ok());
  EXPECT_TRUE(contract.GeneralInterrogationPending(
      "IED1LD0/LLN0$BR$brcb1"));
  auto ordinaryReport = MakeReport(false);
  EXPECT_EQ(contract.MarkGeneralInterrogationComplete(ordinaryReport)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  ASSERT_TRUE(contract.MarkGeneralInterrogationComplete(giReport).ok());
  EXPECT_FALSE(contract.GeneralInterrogationPending(
      "IED1LD0/LLN0$BR$brcb1"));
  EXPECT_TRUE(contract.Ready());

  ASSERT_TRUE(
      contract.MarkGeneralInterrogationRequested("IED1LD0/LLN0$BR$brcb1")
          .ok());
  EXPECT_FALSE(contract.Ready());

  contract.ResetReadiness();
  EXPECT_FALSE(contract.Ready());
}

// 验证多RCB会话必须完成所有RCB的GI后才能整体进入READY。
TEST(IEC61850MmsSessionTest, RequiresGeneralInterrogationForEveryRcb) {
  auto plan = MakePlan();
  auto* secondRcb = plan.ied.add_report_controls();
  *secondRcb = plan.ied.report_controls(0);
  secondRcb->set_rcb_ref("IED1LD0/LLN0$UR$urcb1");
  secondRcb->set_report_id("IED1/Measurements2");
  secondRcb->set_buffered(false);

  auto directory = MakeDirectory();
  auto secondDirectoryRcb = directory.reportControls.front();
  secondDirectoryRcb.rcbRef = "IED1LD0/LLN0$UR$urcb1";
  secondDirectoryRcb.reportId = "IED1/Measurements2";
  secondDirectoryRcb.buffered = false;
  directory.reportControls.emplace_back(std::move(secondDirectoryRcb));

  IEC61850::MmsSessionContract contract(plan);
  ASSERT_TRUE(contract.ValidateOnlineDirectory(directory).ok());
  ASSERT_TRUE(
      contract.MarkRcbEnabled("IED1LD0/LLN0$BR$brcb1", 7).ok());
  ASSERT_TRUE(
      contract.MarkRcbEnabled("IED1LD0/LLN0$UR$urcb1", 7).ok());
  ASSERT_TRUE(contract.MarkGeneralInterrogationRequested(
                              "IED1LD0/LLN0$BR$brcb1")
                  .ok());
  ASSERT_TRUE(contract.MarkGeneralInterrogationRequested(
                              "IED1LD0/LLN0$UR$urcb1")
                  .ok());

  ASSERT_TRUE(contract.MarkGeneralInterrogationComplete(MakeReport(true)).ok());
  EXPECT_FALSE(contract.Ready());
  EXPECT_TRUE(contract.GeneralInterrogationPending(
      "IED1LD0/LLN0$UR$urcb1"));

  auto secondReport = MakeReport(true);
  secondReport.reportRef = "IED1LD0/LLN0$UR$urcb1";
  ASSERT_TRUE(contract.MarkGeneralInterrogationComplete(secondReport).ok());
  EXPECT_TRUE(contract.Ready());
}

// 验证：重连后重新核对在线目录会清除旧RCB/GI就绪状态，不能复用旧会话Ready。
TEST(IEC61850MmsSessionTest, RevalidatingDirectoryResetsReadiness) {
  const auto plan = MakePlan();
  IEC61850::MmsSessionContract contract(plan);

  ASSERT_TRUE(contract.ValidateOnlineDirectory(MakeDirectory()).ok());
  ASSERT_TRUE(
      contract.MarkRcbEnabled("IED1LD0/LLN0$BR$brcb1", 7).ok());
  ASSERT_TRUE(contract.MarkGeneralInterrogationRequested(
                  "IED1LD0/LLN0$BR$brcb1")
                  .ok());
  ASSERT_TRUE(contract.MarkGeneralInterrogationComplete(MakeReport(true)).ok());
  ASSERT_TRUE(contract.Ready());

  ASSERT_TRUE(contract.ValidateOnlineDirectory(MakeDirectory()).ok());
  EXPECT_FALSE(contract.Ready());
}

// 验证：嵌套数组/结构的原始内容也参与报告分段内存预算，超限时整段丢弃。
TEST(IEC61850MmsSessionTest, RejectsOversizedStructuredReportSegment) {
  IEC61850::MmsReportSegment segment;
  segment.reportRef = "IED1LD0/LLN0$BR$brcb1";
  segment.dataSetRef = "IED1LD0/LLN0$measurements";
  segment.confRev = 7;
  segment.sequenceNumber = 1;
  segment.segmentNumber = 0;

  auto composite = std::make_shared<IEC61850::MmsCompositeValue>();
  composite->kind = IEC61850::MmsCompositeValue::Kind::STRUCTURE;
  composite->encodedContent.resize(
      mskdsp::kIec61850MaxMmsVariableValueBytes + 1, 0xa5);
  segment.values.push_back(
      {"IED1LD0/MMXU1.TotW.mag.f",
       IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX,
       std::shared_ptr<IEC61850::MmsCompositeValue>(std::move(composite))});

  IEC61850::MmsReportAssembler assembler;
  EXPECT_FALSE(assembler.Push(std::move(segment), 100).has_value());
}

// 验证GI报告未覆盖完整DataSet时不能把会话标记为Ready。
TEST(IEC61850MmsSessionTest, RejectsPartialGeneralInterrogationReport) {
  auto plan = MakePlan();
  auto* secondAttribute = plan.ied.add_data_attributes();
  secondAttribute->set_data_ref("IED1LD0/MMXU1.TotW.mag.i");
  secondAttribute->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  auto* secondMember = plan.ied.mutable_data_sets(0)->add_members();
  secondMember->set_data_ref(secondAttribute->data_ref());
  secondMember->set_fc(secondAttribute->fc());
  auto directory = MakeDirectory();
  directory.dataAttributes.push_back(
      {secondAttribute->data_ref(), secondAttribute->fc()});
  directory.dataSets.front().members.push_back(
      {secondAttribute->data_ref(), secondAttribute->fc()});

  IEC61850::MmsSessionContract contract(plan);
  ASSERT_TRUE(contract.ValidateOnlineDirectory(directory).ok());
  ASSERT_TRUE(
      contract.MarkRcbEnabled("IED1LD0/LLN0$BR$brcb1", 7).ok());
  ASSERT_TRUE(contract.MarkGeneralInterrogationRequested(
                  "IED1LD0/LLN0$BR$brcb1")
                  .ok());

  EXPECT_EQ(contract.MarkGeneralInterrogationComplete(MakeReport(true))
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_FALSE(contract.Ready());
}

// 验证GI报告不仅要覆盖完整DataSet，还必须保持与在线DataSet一致的成员顺序。
TEST(IEC61850MmsSessionTest, RejectsGeneralInterrogationMemberOrderMismatch) {
  auto plan = MakePlan();
  auto* secondAttribute = plan.ied.add_data_attributes();
  secondAttribute->set_data_ref("IED1LD0/MMXU1.TotW.mag.i");
  secondAttribute->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  secondAttribute->set_basic_type("FLOAT32");
  secondAttribute->set_count(1);
  auto* secondMember = plan.ied.mutable_data_sets(0)->add_members();
  secondMember->set_data_ref(secondAttribute->data_ref());
  secondMember->set_fc(secondAttribute->fc());

  auto directory = MakeDirectory();
  directory.dataAttributes.push_back(
      {secondAttribute->data_ref(), secondAttribute->fc()});
  directory.dataSets.front().members.push_back(
      {secondAttribute->data_ref(), secondAttribute->fc()});

  IEC61850::MmsSessionContract contract(plan);
  ASSERT_TRUE(contract.ValidateOnlineDirectory(directory).ok());
  ASSERT_TRUE(
      contract.MarkRcbEnabled("IED1LD0/LLN0$BR$brcb1", 7).ok());
  ASSERT_TRUE(contract.MarkGeneralInterrogationRequested(
                  "IED1LD0/LLN0$BR$brcb1")
                  .ok());

  auto report = MakeReport(true);
  report.values.push_back(
      {secondAttribute->data_ref(), secondAttribute->fc(), std::int64_t{2}});
  std::swap(report.values[0], report.values[1]);
  EXPECT_EQ(contract.MarkGeneralInterrogationComplete(report).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_FALSE(contract.Ready());
}

// 验证：完整分段报告只有在最后一段到达后才交付，且保留段内数据顺序。
TEST(IEC61850MmsSessionTest, AssemblesReportSegments) {
  IEC61850::MmsReportAssembler assembler;
  IEC61850::MmsReportSegment first;
  first.reportRef = "IED1/Report";
  first.dataSetRef = "IED1/DS";
  first.confRev = 3;
  first.sequenceNumber = 10;
  first.segmentNumber = 0;
  first.moreSegmentsFollow = true;
  first.generalInterrogation = true;
  first.receiveTimestampMs = 100;
  first.values.push_back({"IED1/Trip", IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST,
                          true, {}, 100, true});
  EXPECT_FALSE(assembler.Push(std::move(first), 100).has_value());

  IEC61850::MmsReportSegment last;
  last.reportRef = "IED1/Report";
  last.dataSetRef = "IED1/DS";
  last.confRev = 3;
  last.sequenceNumber = 10;
  last.segmentNumber = 1;
  last.moreSegmentsFollow = false;
  last.generalInterrogation = false;
  last.receiveTimestampMs = 101;
  last.values.push_back({"IED1/Quality", IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST,
                         false, {}, 101, true});
  const auto report = assembler.Push(std::move(last), 101);

  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report->sequenceNumber, 10u);
  EXPECT_TRUE(report->generalInterrogation);
  ASSERT_EQ(report->values.size(), 2u);
  EXPECT_EQ(report->values[0].dataRef, "IED1/Trip");
  EXPECT_EQ(report->values[1].dataRef, "IED1/Quality");
}

// 验证：分段缺号、重复段和组超时都会丢弃整组报告，后续新组仍可继续接收。
TEST(IEC61850MmsSessionTest, DropsInvalidOrExpiredReportSegments) {
  IEC61850::MmsReportAssembler assembler(10);
  IEC61850::MmsReportSegment first;
  first.reportRef = "IED1/Report";
  first.dataSetRef = "IED1/DS";
  first.confRev = 3;
  first.sequenceNumber = 10;
  first.segmentNumber = 0;
  first.moreSegmentsFollow = true;
  EXPECT_FALSE(assembler.Push(first, 100).has_value());

  auto duplicate = first;
  EXPECT_FALSE(assembler.Push(std::move(duplicate), 101).has_value());

  first.sequenceNumber = 11;
  ASSERT_FALSE(assembler.Push(first, 102).has_value());
  auto missing = first;
  missing.segmentNumber = 2;
  EXPECT_FALSE(assembler.Push(std::move(missing), 103).has_value());

  first.sequenceNumber = 12;
  ASSERT_FALSE(assembler.Push(first, 104).has_value());
  auto changedRevision = first;
  changedRevision.segmentNumber = 1;
  changedRevision.confRev = 4;
  changedRevision.moreSegmentsFollow = false;
  EXPECT_FALSE(assembler.Push(std::move(changedRevision), 105).has_value());

  first.sequenceNumber = 13;
  ASSERT_FALSE(assembler.Push(first, 106).has_value());
  assembler.Expire(116);
  auto late = first;
  late.segmentNumber = 1;
  late.moreSegmentsFollow = false;
  EXPECT_FALSE(assembler.Push(std::move(late), 117).has_value());

  first.sequenceNumber = 14;
  first.moreSegmentsFollow = false;
  EXPECT_TRUE(assembler.Push(std::move(first), 118).has_value());
}

// 验证A通道失败后才完成建链的B通道会被安排重新执行RCB配置。
TEST(IEC61850MmsChannelPolicyTest, SchedulesLateConnectedStandbyChannel) {
  const std::vector<IEC61850::MmsChannelStatus> channels{
      {IEC61850Proto::NETWORK_CHANNEL_A,
       IEC61850Proto::CHANNEL_STATE_ERROR, "A连接失败"},
      {IEC61850Proto::NETWORK_CHANNEL_B,
       IEC61850Proto::CHANNEL_STATE_CONNECTED, {}}};

  EXPECT_TRUE(IEC61850::ShouldScheduleRcbReconfigurationAfterConnect(
      IEC61850Proto::NETWORK_CHANNEL_B, channels, std::nullopt));
}

// 验证较低编号通道仍在连接或已经持有配置权时，备用通道不会提前抢占RCB。
TEST(IEC61850MmsChannelPolicyTest, DefersStandbyWhilePreferredChannelPending) {
  const std::vector<IEC61850::MmsChannelStatus> channels{
      {IEC61850Proto::NETWORK_CHANNEL_A,
       IEC61850Proto::CHANNEL_STATE_CONNECTING, {}},
      {IEC61850Proto::NETWORK_CHANNEL_B,
       IEC61850Proto::CHANNEL_STATE_CONNECTED, {}}};

  EXPECT_FALSE(IEC61850::ShouldScheduleRcbReconfigurationAfterConnect(
      IEC61850Proto::NETWORK_CHANNEL_B, channels, std::nullopt));
  EXPECT_FALSE(IEC61850::ShouldScheduleRcbReconfigurationAfterConnect(
      IEC61850Proto::NETWORK_CHANNEL_B, channels,
      IEC61850Proto::NETWORK_CHANNEL_A));
}

// 验证最低优先级通道正常建链时不会因为没有配置权而无条件重建会话。
TEST(IEC61850MmsChannelPolicyTest, DoesNotReschedulePreferredChannel) {
  const std::vector<IEC61850::MmsChannelStatus> channels{
      {IEC61850Proto::NETWORK_CHANNEL_A,
       IEC61850Proto::CHANNEL_STATE_CONNECTED, {}}};

  EXPECT_FALSE(IEC61850::ShouldScheduleRcbReconfigurationAfterConnect(
      IEC61850Proto::NETWORK_CHANNEL_A, channels, std::nullopt));
}

// 验证：超大单值、超大逻辑报告和过多在途组会被有界处理，后续有效组仍可完成。
TEST(IEC61850MmsSessionTest, BoundsPendingReportMemory) {
  IEC61850::MmsReportAssembler assembler;
  IEC61850::MmsReportSegment oversizedValue;
  oversizedValue.reportRef = "IED1/Report";
  oversizedValue.dataSetRef = "IED1/DS";
  oversizedValue.confRev = 3;
  oversizedValue.sequenceNumber = 1;
  oversizedValue.moreSegmentsFollow = true;
  oversizedValue.values.push_back(
      {"IED1/Text", IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST,
       std::string(mskdsp::kIec61850MaxMmsVariableValueBytes + 1, 'x')});
  EXPECT_FALSE(assembler.Push(std::move(oversizedValue), 100).has_value());

  IEC61850::MmsReportSegment oversizedReport;
  oversizedReport.reportRef = "IED1/Report";
  oversizedReport.dataSetRef = "IED1/DS";
  oversizedReport.confRev = 3;
  oversizedReport.sequenceNumber = 2;
  oversizedReport.moreSegmentsFollow = true;
  for (int index = 0; index < 17; ++index) {
    oversizedReport.values.push_back(
        {"IED1/Text" + std::to_string(index),
         IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST,
         std::string(mskdsp::kIec61850MaxMmsVariableValueBytes, 'x')});
  }
  EXPECT_FALSE(assembler.Push(std::move(oversizedReport), 101).has_value());

  for (std::size_t index = 0;
       index <= mskdsp::kIec61850MaxMmsPendingReportGroups; ++index) {
    IEC61850::MmsReportSegment pending;
    pending.reportRef = "IED1/Report";
    pending.dataSetRef = "IED1/DS";
    pending.confRev = 3;
    pending.sequenceNumber = 100 + index;
    pending.moreSegmentsFollow = true;
    EXPECT_FALSE(assembler.Push(std::move(pending), 200).has_value());
  }

  IEC61850::MmsReportSegment evictedLast;
  evictedLast.reportRef = "IED1/Report";
  evictedLast.dataSetRef = "IED1/DS";
  evictedLast.confRev = 3;
  evictedLast.sequenceNumber = 100;
  evictedLast.segmentNumber = 1;
  EXPECT_FALSE(assembler.Push(std::move(evictedLast), 201).has_value());

  IEC61850::MmsReportSegment newestLast;
  newestLast.reportRef = "IED1/Report";
  newestLast.dataSetRef = "IED1/DS";
  newestLast.confRev = 3;
  newestLast.sequenceNumber =
      100 + mskdsp::kIec61850MaxMmsPendingReportGroups;
  newestLast.segmentNumber = 1;
  EXPECT_TRUE(assembler.Push(std::move(newestLast), 201).has_value());
}

// 验证：接近int64上限的接收时间使用饱和截止时刻，不发生有符号溢出。
TEST(IEC61850MmsSessionTest, SaturatesSegmentExpirationDeadline) {
  IEC61850::MmsReportAssembler assembler(10);
  IEC61850::MmsReportSegment first;
  first.reportRef = "IED1/Report";
  first.dataSetRef = "IED1/DS";
  first.confRev = 3;
  first.sequenceNumber = 1;
  first.moreSegmentsFollow = true;
  const auto nearMaximum = std::numeric_limits<std::int64_t>::max() - 5;
  ASSERT_FALSE(assembler.Push(first, nearMaximum).has_value());

  assembler.Expire(std::numeric_limits<std::int64_t>::max());
  first.segmentNumber = 1;
  first.moreSegmentsFollow = false;
  EXPECT_FALSE(assembler
                   .Push(std::move(first),
                         std::numeric_limits<std::int64_t>::max())
                   .has_value());
}

}  // namespace
