#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "IEC61850RealtimePlan.h"

namespace {

IEC61850Proto::NormalizedSclModel MakeSvModel(bool includeAddress = true,
                                              std::uint32_t nofAsdu = 1,
                                              std::string_view basicType =
                                                  "INT32") {
  IEC61850Proto::NormalizedSclModel model;
  auto* publisher = model.add_ieds();
  publisher->set_name("MU");
  auto* dataSet = publisher->add_data_sets();
  dataSet->set_access_point("AP1");
  dataSet->set_data_set_ref("MULD0/LLN0$dataset");
  auto* member = dataSet->add_members();
  member->set_data_ref("MULD0/TCTR1.Amp.instMag.i");
  member->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  auto* attribute = publisher->add_data_attributes();
  attribute->set_access_point("AP1");
  attribute->set_data_ref(member->data_ref());
  attribute->set_fc(member->fc());
  attribute->set_basic_type(std::string(basicType));
  auto* control = publisher->add_sampled_value_controls();
  control->set_access_point("AP1");
  control->set_control_ref("MULD0/LLN0$MS$smv1");
  control->set_data_set_ref(dataSet->data_set_ref());
  control->set_sv_id("MU01");
  control->set_config_revision(5);
  control->set_sample_rate(80);
  control->set_nof_asdu(nofAsdu);
  if (includeAddress) {
    auto* connected = model.add_connected_access_points();
    connected->set_ied_name("MU");
    connected->set_ap_name("AP1");
    connected->set_subnetwork_name("process");
    auto* smv = connected->add_smv();
    smv->set_ld_inst("LD0");
    smv->set_cb_name("smv1");
    smv->set_mac_address("01-0C-CD-04-00-01");
    smv->set_app_id(0x4001);
    smv->set_vlan_id(7);
    smv->set_vlan_priority(4);
  }
  return model;
}

IEC61850::ProtocolIedPlan MakeSvPlan() {
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("protection-1");
  plan.config.set_nominal_frequency_hz(60.0);
  plan.config.set_enable_sv(true);
  plan.ied.set_name("PROT");
  auto* extRef = plan.ied.add_ext_refs();
  extRef->set_service_type("SMV");
  extRef->set_int_addr("sample-input");
  extRef->set_source_data_ref("MULD0/TCTR1.Amp.instMag.i");
  extRef->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  extRef->set_ied_name("MU");
  extRef->set_src_ld_inst("LD0");
  extRef->set_src_ln_class("LLN0");
  extRef->set_src_cb_name("smv1");
  auto& binding = plan.networkBindings.emplace_back();
  binding.channel.set_channel(IEC61850Proto::NETWORK_CHANNEL_A);
  binding.channel.set_enabled(true);
  binding.channel.set_interface_name("eth1");
  binding.connectedAccessPoint.set_subnetwork_name("process");
  return plan;
}

IEC61850Proto::NormalizedSclModel MakeGoosePublisherModel(
    bool includeAddress = true) {
  IEC61850Proto::NormalizedSclModel model;
  auto* ied = model.add_ieds();
  ied->set_name("IED1");
  auto* dataSet = ied->add_data_sets();
  dataSet->set_access_point("AP1");
  dataSet->set_data_set_ref("IED1LD0/LLN0$trip");
  auto* member = dataSet->add_members();
  member->set_data_ref("IED1LD0/PTRC1.Tr.general");
  member->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  auto* attribute = ied->add_data_attributes();
  attribute->set_access_point("AP1");
  attribute->set_data_ref(member->data_ref());
  attribute->set_fc(member->fc());
  attribute->set_basic_type("BOOLEAN");
  auto* control = ied->add_gse_controls();
  control->set_access_point("AP1");
  control->set_owner_node_ref("IED1LD0/LLN0");
  control->set_name("gcb1");
  control->set_control_ref("IED1LD0/LLN0$GO$gcb1");
  control->set_data_set_ref(dataSet->data_set_ref());
  control->set_go_id("Trip");
  control->set_config_revision(4);
  if (includeAddress) {
    auto* connected = model.add_connected_access_points();
    connected->set_ied_name("IED1");
    connected->set_ap_name("AP1");
    connected->set_subnetwork_name("station");
    auto* gse = connected->add_gse();
    gse->set_ld_inst("LD0");
    gse->set_cb_name("gcb1");
    gse->set_mac_address("01-0C-CD-01-00-01");
    gse->set_app_id(0x1001);
    gse->set_vlan_id(1);
    gse->set_vlan_priority(4);
  }
  return model;
}

IEC61850::ProtocolIedPlan MakeGoosePublisherPlan(
    const IEC61850Proto::NormalizedSclModel& model) {
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("protection-1");
  plan.config.set_enable_goose(true);
  plan.ied = model.ieds(0);
  auto& binding = plan.networkBindings.emplace_back();
  binding.channel.set_channel(IEC61850Proto::NETWORK_CHANNEL_A);
  binding.channel.set_enabled(true);
  binding.channel.set_interface_name("eth1");
  binding.connectedAccessPoint.set_subnetwork_name("station");
  return plan;
}

}  // namespace

// 验证SMV外部引用被编译为包含身份、布局和二层端点的SV启动计划。
TEST(IEC61850RealtimePlanTest, BuildsSvStreamFromExtRef) {
  const auto model = MakeSvModel();
  auto plan = MakeSvPlan();
  IEC61850Proto::PointMappings mappings;

  const auto status =
      IEC61850::BuildRealtimeProtocolPlan(model, mappings, &plan);

  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(plan.svStreams.size(), 1u);
  const auto& stream = plan.svStreams.front();
  EXPECT_EQ(stream.streamId, 1u);
  EXPECT_EQ(stream.controlRef, "MULD0/LLN0$MS$smv1");
  EXPECT_EQ(stream.svId, "MU01");
  EXPECT_EQ(stream.configRevision, 5u);
  EXPECT_EQ(stream.sampleRate, 80u);
  EXPECT_DOUBLE_EQ(stream.nominalFrequencyHz, 60.0);
  ASSERT_EQ(stream.members.size(), 1u);
  EXPECT_EQ(stream.members.front().signalId, 1u);
  EXPECT_EQ(stream.members.front().valueType,
            IEC61850Proto::POINT_VALUE_TYPE_INT64);
  ASSERT_EQ(stream.derivedMembers.size(), 1u);
  EXPECT_EQ(stream.derivedMembers.front().inputSignalId, 1u);
  EXPECT_EQ(stream.derivedMembers.front().rmsSignalId, 2u);
  EXPECT_EQ(stream.derivedMembers.front().rmsDataRef,
            "SV_DERIVED/1/RMS/MULD0/TCTR1.Amp.instMag.i");
  ASSERT_EQ(plan.realtimeSignals.size(), 2u);
  EXPECT_EQ(plan.realtimeSignals[1].dataRef,
            "SV_DERIVED/1/RMS/MULD0/TCTR1.Amp.instMag.i");
  EXPECT_EQ(plan.realtimeSignals[1].valueType,
            IEC61850Proto::POINT_VALUE_TYPE_DOUBLE);
  ASSERT_EQ(stream.endpoints.size(), 1u);
  EXPECT_EQ(stream.endpoints.front().appId, 0x4001u);
  EXPECT_EQ(stream.endpoints.front().vlanId, 7u);
}

// 验证SV发布端缺少Communication/SMV地址时启动计划明确失败。
TEST(IEC61850RealtimePlanTest, RejectsSvStreamWithoutMulticastAddress) {
  const auto model = MakeSvModel(false);
  auto plan = MakeSvPlan();
  IEC61850Proto::PointMappings mappings;

  const auto status =
      IEC61850::BuildRealtimeProtocolPlan(model, mappings, &plan);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("SV发布端缺少有效MAC"),
            std::string::npos);
}

// 验证首期SV数学计划只为单ASDU流生成RMS派生量，多ASDU流仍可接收原始样本。
TEST(IEC61850RealtimePlanTest, DoesNotBuildMathPlanForMultipleAsdus) {
  const auto model = MakeSvModel(true, 2);
  auto plan = MakeSvPlan();
  IEC61850Proto::PointMappings mappings;

  const auto status =
      IEC61850::BuildRealtimeProtocolPlan(model, mappings, &plan);

  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(plan.svStreams.size(), 1u);
  EXPECT_EQ(plan.svStreams.front().nofAsdu, 2u);
  EXPECT_TRUE(plan.svStreams.front().derivedMembers.empty());
}

// 验证BOOL采样成员仍可接收，但不会被错误地送入RMS数学链路。
TEST(IEC61850RealtimePlanTest, SkipsBooleanMemberForMathPlan) {
  const auto model = MakeSvModel(true, 1, "BOOLEAN");
  auto plan = MakeSvPlan();
  IEC61850Proto::PointMappings mappings;

  const auto status =
      IEC61850::BuildRealtimeProtocolPlan(model, mappings, &plan);

  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(plan.svStreams.size(), 1u);
  ASSERT_EQ(plan.svStreams.front().members.size(), 1u);
  EXPECT_EQ(plan.svStreams.front().members.front().valueType,
            IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  EXPECT_TRUE(plan.svStreams.front().derivedMembers.empty());
}

// 验证SV派生点映射按稳定data_ref和FC绑定到启动计划，并保留点标签。
TEST(IEC61850RealtimePlanTest, ResolvesSvDerivedPointMapping) {
  const auto model = MakeSvModel();
  auto plan = MakeSvPlan();
  IEC61850Proto::PointMappings mappings;
  auto* point = mappings.add_points();
  point->set_tag("RMS");
  point->set_data_ref("SV_DERIVED/1/RMS/MULD0/TCTR1.Amp.instMag.i");
  point->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  point->set_source(IEC61850Proto::POINT_SOURCE_SV_DERIVED);
  point->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_DOUBLE);

  const auto status =
      IEC61850::BuildRealtimeProtocolPlan(model, mappings, &plan);

  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(plan.realtimeSignals.size(), 2u);
  EXPECT_EQ(plan.realtimeSignals.back().tag, "RMS");
}

// 验证SV派生点映射引用不存在或类型不符时在启动计划阶段失败。
TEST(IEC61850RealtimePlanTest, RejectsUnknownSvDerivedPointMapping) {
  const auto model = MakeSvModel();
  auto plan = MakeSvPlan();
  IEC61850Proto::PointMappings mappings;
  auto* point = mappings.add_points();
  point->set_data_ref("SV_DERIVED/1/RMS/MULD0/TCTR1.Amp.unknown");
  point->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  point->set_source(IEC61850Proto::POINT_SOURCE_SV_DERIVED);
  point->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_DOUBLE);

  const auto status =
      IEC61850::BuildRealtimeProtocolPlan(model, mappings, &plan);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("SV派生点映射未匹配"),
            std::string::npos);
}

// 验证本地GSEControl从目标IED模型生成独立GOOSE发布计划，不依赖ExtRef订阅。
TEST(IEC61850RealtimePlanTest, BuildsGoosePublisherFromLocalGseControl) {
  const auto model = MakeGoosePublisherModel();
  auto plan = MakeGoosePublisherPlan(model);
  IEC61850Proto::PointMappings mappings;

  const auto status =
      IEC61850::BuildRealtimeProtocolPlan(model, mappings, &plan);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(plan.gooseSubscriptions.empty());
  ASSERT_EQ(plan.goosePublishers.size(), 1u);
  const auto& publisher = plan.goosePublishers.front();
  EXPECT_EQ(publisher.publisherId, 1u);
  EXPECT_EQ(publisher.publisherIed, "IED1");
  EXPECT_EQ(publisher.controlRef, "IED1LD0/LLN0$GO$gcb1");
  EXPECT_EQ(publisher.dataSetRef, "IED1LD0/LLN0$trip");
  EXPECT_EQ(publisher.goId, "Trip");
  EXPECT_EQ(publisher.configRevision, 4u);
  ASSERT_EQ(publisher.members.size(), 1u);
  EXPECT_EQ(publisher.members.front().dataRef,
            "IED1LD0/PTRC1.Tr.general");
  EXPECT_EQ(publisher.members.front().valueType,
            IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  ASSERT_EQ(publisher.endpoints.size(), 1u);
  EXPECT_EQ(publisher.endpoints.front().appId, 0x1001u);
  EXPECT_EQ(publisher.endpoints.front().vlanId, 1u);
}

// 验证本地GSEControl缺少Communication组播地址时不能生成发布计划。
TEST(IEC61850RealtimePlanTest, RejectsGoosePublisherWithoutMulticastAddress) {
  const auto model = MakeGoosePublisherModel(false);
  auto plan = MakeGoosePublisherPlan(model);
  IEC61850Proto::PointMappings mappings;

  const auto status =
      IEC61850::BuildRealtimeProtocolPlan(model, mappings, &plan);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("GOOSE本地发布端缺少有效MAC"),
            std::string::npos);
}
