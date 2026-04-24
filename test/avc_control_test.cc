#include <gtest/gtest.h>

#include <vector>

#include "AVCControl.h"

namespace {

AVCProto::GroupConfig MakeBaseConfig() {
  AVCProto::GroupConfig cfg;
  cfg.set_group_name("g-1");
  cfg.mutable_voltage_meas()->set_tag("V_MEAS");
  cfg.mutable_voltage_meas()->set_unit("pu");
  cfg.mutable_voltage_cmd()->set_tag("V_CMD");
  cfg.mutable_voltage_cmd()->set_unit("pu");
  cfg.mutable_voltage_control()->set_kp(100.0);
  cfg.mutable_voltage_control()->set_deadband(0.0);

  auto* m1 = cfg.add_members();
  m1->set_member_name("m1");
  m1->set_controllable(true);
  m1->set_weight(1.0);
  m1->set_q_min_kvar(0.0);
  m1->set_q_max_kvar(100.0);
  m1->mutable_q_meas()->set_tag("M1_Q_MEAS");
  m1->mutable_q_meas()->set_unit("kVar");
  m1->mutable_q_set()->mutable_signal()->set_tag("M1_Q_SET");
  m1->mutable_q_set()->mutable_signal()->set_unit("kVar");
  m1->mutable_q_set()->set_mode(AVCProto::VALUE_MODE_ABSOLUTE);

  auto* m2 = cfg.add_members();
  m2->set_member_name("m2");
  m2->set_controllable(true);
  m2->set_weight(1.0);
  m2->set_q_min_kvar(0.0);
  m2->set_q_max_kvar(100.0);
  m2->mutable_q_meas()->set_tag("M2_Q_MEAS");
  m2->mutable_q_meas()->set_unit("kVar");
  m2->mutable_q_set()->mutable_signal()->set_tag("M2_Q_SET");
  m2->mutable_q_set()->mutable_signal()->set_unit("kVar");
  m2->mutable_q_set()->set_mode(AVCProto::VALUE_MODE_ABSOLUTE);

  return cfg;
}

AVC::ControlInput MakeVoltageInput(double voltageCmdRaw, double voltageMeasRaw, const std::vector<double>& qMeasRaw) {
  AVC::ControlInput input;
  input.hasVoltageCmdRaw = true;
  input.voltageCmdRaw = voltageCmdRaw;
  input.hasVoltageMeasRaw = true;
  input.voltageMeasRaw = voltageMeasRaw;
  input.hasMemberQMeasRaw.assign(qMeasRaw.size(), true);
  input.memberQMeasRaw = qMeasRaw;
  return input;
}

AVC::ControlInput MakeQTotalInput(double qCmdRaw, const std::vector<double>& qMeasRaw) {
  AVC::ControlInput input;
  input.hasQTotalCmdRaw = true;
  input.qTotalCmdRaw = qCmdRaw;
  input.hasMemberQMeasRaw.assign(qMeasRaw.size(), true);
  input.memberQMeasRaw = qMeasRaw;
  return input;
}

}  // namespace

// 验证：目标电压模式在超出死区时按电压偏差线性换算总无功目标并分配到成员。
TEST(AvcControlTest, VoltageModeComputesReactiveTargetFromVoltageError) {
  auto cfg = MakeBaseConfig();
  AGVC::WeightedStrategy strategy;
  auto input = MakeVoltageInput(1.00, 0.90, {0.0, 0.0});

  auto out = AVC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(out->hasVoltageError);
  EXPECT_NEAR(out->voltageError, 0.10, 1e-6);
  EXPECT_NEAR(out->desiredTotalQKvar, 10.0, 1e-6);
  EXPECT_NEAR(out->actualTargetQKvar, 10.0, 1e-6);
  ASSERT_EQ(out->memberTargetQKvar.size(), 2u);
  EXPECT_NEAR(out->memberTargetQKvar[0], 5.0, 1e-6);
  EXPECT_NEAR(out->memberTargetQKvar[1], 5.0, 1e-6);
}

// 验证：目标电压模式在死区内保持当前总无功实测，不继续推动总无功目标变化。
TEST(AvcControlTest, VoltageModeKeepsCurrentReactiveWhenInsideDeadband) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_voltage_control()->set_deadband(0.05);
  AGVC::WeightedStrategy strategy;
  auto input = MakeVoltageInput(1.00, 0.98, {10.0, 20.0});

  auto out = AVC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->totalQMeasKvar, 30.0, 1e-6);
  EXPECT_NEAR(out->desiredTotalQKvar, 30.0, 1e-6);
  EXPECT_NEAR(out->actualTargetQKvar, 30.0, 1e-6);
}

// 验证：总无功绝对值命令直接作为总无功目标参与分配。
TEST(AvcControlTest, QTotalAbsoluteCommandAllocatesDirectly) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_q_total_cmd()->mutable_signal()->set_tag("Q_CMD");
  cfg.mutable_q_total_cmd()->mutable_signal()->set_unit("kVar");
  cfg.mutable_q_total_cmd()->set_mode(AVCProto::VALUE_MODE_ABSOLUTE);

  AGVC::WeightedStrategy strategy;
  auto input = MakeQTotalInput(60.0, {10.0, 20.0});

  auto out = AVC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->desiredTotalQKvar, 60.0, 1e-6);
  EXPECT_NEAR(out->actualTargetQKvar, 60.0, 1e-6);
  EXPECT_NEAR(out->totalQErrorKvar, 30.0, 1e-6);
}

// 验证：DELTA_BASE_LAST_TARGET 使用上一轮期望总无功目标值作为基准。
TEST(AvcControlTest, QTotalDeltaBaseLastTargetUsesDesiredTarget) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_q_total_cmd()->mutable_signal()->set_tag("Q_CMD");
  cfg.mutable_q_total_cmd()->mutable_signal()->set_unit("kVar");
  cfg.mutable_q_total_cmd()->set_mode(AVCProto::VALUE_MODE_DELTA);
  cfg.mutable_q_total_cmd()->set_delta_base(AVCProto::DELTA_BASE_LAST_TARGET);

  AGVC::WeightedStrategy strategy;
  auto input = MakeQTotalInput(5.0, {10.0, 10.0});
  input.hasLastDesiredTotalQKvar = true;
  input.lastDesiredTotalQKvar = 50.0;

  auto out = AVC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->desiredTotalQKvar, 55.0, 1e-6);
  EXPECT_NEAR(out->actualTargetQKvar, 55.0, 1e-6);
}

// 验证：DELTA_BASE_CURRENT_MEAS 使用当前总无功实测作为总无功增量基准。
TEST(AvcControlTest, QTotalDeltaBaseCurrentMeasUsesMeasuredTotal) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_q_total_cmd()->mutable_signal()->set_tag("Q_CMD");
  cfg.mutable_q_total_cmd()->mutable_signal()->set_unit("kVar");
  cfg.mutable_q_total_cmd()->set_mode(AVCProto::VALUE_MODE_DELTA);
  cfg.mutable_q_total_cmd()->set_delta_base(AVCProto::DELTA_BASE_CURRENT_MEAS);

  AGVC::WeightedStrategy strategy;
  auto input = MakeQTotalInput(5.0, {10.0, 10.0});

  auto out = AVC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->totalQMeasKvar, 20.0, 1e-6);
  EXPECT_NEAR(out->desiredTotalQKvar, 25.0, 1e-6);
  EXPECT_NEAR(out->actualTargetQKvar, 25.0, 1e-6);
}

// 验证：DELTA_BASE_BASE_TAG 使用同组 conn_id 下的 base_tag 当前值作为总无功增量基准。
TEST(AvcControlTest, QTotalDeltaBaseBaseTagUsesBaseTag) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_q_total_cmd()->mutable_signal()->set_tag("Q_CMD");
  cfg.mutable_q_total_cmd()->mutable_signal()->set_unit("kVar");
  cfg.mutable_q_total_cmd()->set_mode(AVCProto::VALUE_MODE_DELTA);
  cfg.mutable_q_total_cmd()->set_delta_base(AVCProto::DELTA_BASE_BASE_TAG);
  cfg.mutable_q_total_cmd()->set_base_tag("BASE_Q");

  AGVC::WeightedStrategy strategy;
  auto input = MakeQTotalInput(5.0, {0.0, 0.0});
  input.baseRawByTag.emplace("BASE_Q", 30.0);

  auto out = AVC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->desiredTotalQKvar, 35.0, 1e-6);
  EXPECT_NEAR(out->actualTargetQKvar, 35.0, 1e-6);
}

// 验证：成员设定为 DELTA/LAST_TARGET 时发布增量值。
TEST(AvcControlTest, MemberDeltaBaseLastTargetPublishesDelta) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_q_total_cmd()->mutable_signal()->set_tag("Q_CMD");
  cfg.mutable_q_total_cmd()->mutable_signal()->set_unit("kVar");
  cfg.mutable_q_total_cmd()->set_mode(AVCProto::VALUE_MODE_ABSOLUTE);
  cfg.mutable_members(0)->mutable_q_set()->set_mode(AVCProto::VALUE_MODE_DELTA);
  cfg.mutable_members(0)->mutable_q_set()->set_delta_base(AVCProto::DELTA_BASE_LAST_TARGET);
  cfg.mutable_members(1)->mutable_q_set()->set_mode(AVCProto::VALUE_MODE_DELTA);
  cfg.mutable_members(1)->mutable_q_set()->set_delta_base(AVCProto::DELTA_BASE_LAST_TARGET);

  AGVC::WeightedStrategy strategy;
  auto input = MakeQTotalInput(40.0, {10.0, 10.0});
  input.hasLastMemberTargetQKvar = {true, true};
  input.lastMemberTargetQKvar = {15.0, 15.0};

  auto out = AVC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->memberPublishRaw.size(), 2u);
  EXPECT_NEAR(out->memberPublishRaw[0], 5.0, 1e-6);
  EXPECT_NEAR(out->memberPublishRaw[1], 5.0, 1e-6);
}

// 验证：成员设定为 DELTA/CURRENT_MEAS 时，以该成员当前无功实测作为增量基准。
TEST(AvcControlTest, MemberDeltaBaseCurrentMeasPublishesDeltaAgainstMemberMeas) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_q_total_cmd()->mutable_signal()->set_tag("Q_CMD");
  cfg.mutable_q_total_cmd()->mutable_signal()->set_unit("kVar");
  cfg.mutable_q_total_cmd()->set_mode(AVCProto::VALUE_MODE_ABSOLUTE);
  cfg.mutable_members(0)->mutable_q_set()->set_mode(AVCProto::VALUE_MODE_DELTA);
  cfg.mutable_members(0)->mutable_q_set()->set_delta_base(AVCProto::DELTA_BASE_CURRENT_MEAS);
  cfg.mutable_members(1)->mutable_q_set()->set_mode(AVCProto::VALUE_MODE_DELTA);
  cfg.mutable_members(1)->mutable_q_set()->set_delta_base(AVCProto::DELTA_BASE_CURRENT_MEAS);

  AGVC::WeightedStrategy strategy;
  auto input = MakeQTotalInput(40.0, {10.0, 10.0});

  auto out = AVC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->memberPublishRaw.size(), 2u);
  EXPECT_NEAR(out->memberTargetQKvar[0], 20.0, 1e-6);
  EXPECT_NEAR(out->memberTargetQKvar[1], 20.0, 1e-6);
  EXPECT_NEAR(out->memberPublishRaw[0], 10.0, 1e-6);
  EXPECT_NEAR(out->memberPublishRaw[1], 10.0, 1e-6);
}

// 验证：成员总无功能力受限时返回 unallocated，并把总无功目标钳制在当前能力范围内。
TEST(AvcControlTest, AllocationReturnsUnallocatedWhenMembersSaturated) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_q_total_cmd()->mutable_signal()->set_tag("Q_CMD");
  cfg.mutable_q_total_cmd()->mutable_signal()->set_unit("kVar");
  cfg.mutable_q_total_cmd()->set_mode(AVCProto::VALUE_MODE_ABSOLUTE);
  cfg.mutable_members(0)->set_q_max_kvar(10.0);
  cfg.mutable_members(1)->set_q_max_kvar(10.0);

  AGVC::WeightedStrategy strategy;
  auto input = MakeQTotalInput(50.0, {0.0, 0.0});

  auto out = AVC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->desiredTotalQKvar, 20.0, 1e-6);
  EXPECT_NEAR(out->actualTargetQKvar, 20.0, 1e-6);
  EXPECT_NEAR(out->unallocatedQKvar, 0.0, 1e-6);
}

// 验证：不可控成员作为被动无功参与总无功实测和动态上下限计算，但不参与分配。
TEST(AvcControlTest, UncontrollableMemberActsAsPassiveReactiveOutput) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_q_total_cmd()->mutable_signal()->set_tag("Q_CMD");
  cfg.mutable_q_total_cmd()->mutable_signal()->set_unit("kVar");
  cfg.mutable_q_total_cmd()->set_mode(AVCProto::VALUE_MODE_ABSOLUTE);
  cfg.mutable_members(0)->set_controllable(false);

  AGVC::WeightedStrategy strategy;
  auto input = MakeQTotalInput(60.0, {20.0, 0.0});

  auto out = AVC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->passiveQKvar, 20.0, 1e-6);
  EXPECT_NEAR(out->targetControllableQKvar, 40.0, 1e-6);
  EXPECT_NEAR(out->memberTargetQKvar[1], 40.0, 1e-6);
}

// 验证：默认上下限点按当前控制口径计算，缺测的不可控成员按 0 参与动态上下限并置 BAD 质量。
TEST(AvcControlTest, DefaultPointOutputUsesZeroFallbackForMissingUncontrollableMember) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_controllable(false);
  cfg.mutable_members(1)->set_q_min_kvar(10.0);
  cfg.mutable_members(1)->set_q_max_kvar(80.0);

  AVC::ControlInput input;
  input.hasMemberQMeasRaw = {false, false};
  input.memberQMeasRaw = {0.0, 0.0};

  const auto out = AVC::ComputeDefaultPointOutput(cfg, input);
  EXPECT_NEAR(out.theoreticalLowerQKvar, 10.0, 1e-6);
  EXPECT_NEAR(out.theoreticalUpperQKvar, 80.0, 1e-6);
  EXPECT_NEAR(out.dynamicLowerQKvar, 10.0, 1e-6);
  EXPECT_NEAR(out.dynamicUpperQKvar, 80.0, 1e-6);
  EXPECT_EQ(out.dynamicQuality, DataCenterProto::QUALITY_BAD);
}
