#include <gtest/gtest.h>

#include <vector>

#include "AGCControl.h"

namespace {
AGCProto::GroupConfig MakeBaseConfig() {
  AGCProto::GroupConfig cfg;
  cfg.set_group_name("g-1");
  cfg.mutable_p_cmd()->mutable_signal()->set_tag("P_CMD");
  cfg.mutable_p_cmd()->mutable_signal()->set_unit("kW");
  cfg.mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  auto* outputs = cfg.mutable_outputs();
  outputs->mutable_p_total_meas()->set_tag("P_TOTAL_MEAS");
  outputs->mutable_p_total_meas()->set_unit("kW");
  outputs->mutable_p_total_target()->set_tag("P_TOTAL_TARGET");
  outputs->mutable_p_total_target()->set_unit("kW");
  outputs->mutable_p_total_error()->set_tag("P_TOTAL_ERROR");
  outputs->mutable_p_total_error()->set_unit("kW");

  auto* m1 = cfg.add_members();
  m1->set_member_name("m1");
  m1->set_controllable(true);
  m1->set_capacity_kw(100);
  m1->set_weight(1);
  m1->mutable_p_meas()->set_tag("M1_MEAS");
  m1->mutable_p_meas()->set_unit("kW");
  m1->mutable_p_set()->mutable_signal()->set_tag("M1_SET");
  m1->mutable_p_set()->mutable_signal()->set_unit("kW");
  m1->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  auto* m2 = cfg.add_members();
  m2->set_member_name("m2");
  m2->set_controllable(true);
  m2->set_capacity_kw(100);
  m2->set_weight(2);
  m2->mutable_p_meas()->set_tag("M2_MEAS");
  m2->mutable_p_meas()->set_unit("kW");
  m2->mutable_p_set()->mutable_signal()->set_tag("M2_SET");
  m2->mutable_p_set()->mutable_signal()->set_unit("kW");
  m2->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  return cfg;
}

AGC::ControlInput MakeBaseInput(double cmdRaw, const std::vector<double>& measRaw) {
  AGC::ControlInput input;
  input.hasCmdRaw = true;
  input.cmdRaw = cmdRaw;
  input.hasMemberMeasRaw.assign(measRaw.size(), true);
  input.memberMeasRaw = measRaw;
  return input;
}
}  // 命名空间结束

// 验证：绝对总设定按权重分配，并输出派生点与成员设定。
TEST(AgcControlTest, AbsoluteCommandAllocatesByWeight) {
  auto cfg = MakeBaseConfig();
  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(60.0, {10.0, 20.0});

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->totalMeasKw, 30.0, 1e-6);
  EXPECT_NEAR(out->desiredTotalKw, 60.0, 1e-6);
  EXPECT_NEAR(out->actualTargetKw, 60.0, 1e-6);
  ASSERT_EQ(out->memberTargetKw.size(), 2u);
  EXPECT_NEAR(out->memberTargetKw[0], 20.0, 1e-6);
  EXPECT_NEAR(out->memberTargetKw[1], 40.0, 1e-6);
  EXPECT_TRUE(out->publishTotalMeas);
  EXPECT_TRUE(out->publishTotalTarget);
  EXPECT_TRUE(out->publishTotalError);
  ASSERT_EQ(out->memberPublishKw.size(), 2u);
  EXPECT_NEAR(out->memberPublishKw[0], 20.0, 1e-6);
  EXPECT_NEAR(out->memberPublishKw[1], 40.0, 1e-6);
  EXPECT_TRUE(out->hasLastDesiredTotalKw);
  EXPECT_NEAR(out->nextLastDesiredTotalKw, 60.0, 1e-6);
}

// 验证：DELTA_BASE_LAST_TARGET 使用上一轮期望总目标值作为基准。
TEST(AgcControlTest, DeltaBaseLastTargetUsesDesiredTotal) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_DELTA);
  cfg.mutable_p_cmd()->set_delta_base(AGCProto::DELTA_BASE_LAST_TARGET);

  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(5.0, {10.0, 10.0});
  input.hasLastDesiredTotalKw = true;
  input.lastDesiredTotalKw = 50.0;

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->desiredTotalKw, 55.0, 1e-6);
  EXPECT_NEAR(out->actualTargetKw, 55.0, 1e-6);
}

// 验证：成员设定为 DELTA/LAST_TARGET 时发布工程量增量值。
TEST(AgcControlTest, MemberDeltaBaseLastTargetPublishesDelta) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);
  cfg.mutable_members(0)->set_weight(1);
  cfg.mutable_members(1)->set_weight(1);
  cfg.mutable_members(0)->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_DELTA);
  cfg.mutable_members(0)->mutable_p_set()->set_delta_base(AGCProto::DELTA_BASE_LAST_TARGET);
  cfg.mutable_members(1)->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_DELTA);
  cfg.mutable_members(1)->mutable_p_set()->set_delta_base(AGCProto::DELTA_BASE_LAST_TARGET);

  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(40.0, {10.0, 10.0});
  input.hasLastMemberTargetKw = {true, true};
  input.lastMemberTargetKw = {15.0, 15.0};

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->memberPublishKw.size(), 2u);
  EXPECT_NEAR(out->memberPublishKw[0], 5.0, 1e-6);
  EXPECT_NEAR(out->memberPublishKw[1], 5.0, 1e-6);
}

// 验证：成员上限受限时返回 unallocated。
TEST(AgcControlTest, AllocationUnallocatedWhenMembersSaturated) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);
  cfg.mutable_members(0)->set_max_kw(10.0);
  cfg.mutable_members(1)->set_max_kw(10.0);

  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(50.0, {0.0, 0.0});

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_GT(out->unallocatedKw, 0.0);
  EXPECT_NEAR(out->actualTargetKw, 20.0, 1e-6);
}

// 验证：成员下限受限时仍会抬高目标到最小出力约束。
TEST(AgcControlTest, AllocationHonorsMemberMinLimits) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_min_kw(10.0);
  cfg.mutable_members(1)->set_min_kw(20.0);

  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(0.0, {0.0, 0.0});

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->memberTargetKw.size(), 2u);
  EXPECT_NEAR(out->memberTargetKw[0], 10.0, 1e-6);
  EXPECT_NEAR(out->memberTargetKw[1], 20.0, 1e-6);
  EXPECT_LT(out->unallocatedKw, 0.0);
  EXPECT_NEAR(out->actualTargetKw, 30.0, 1e-6);
}

// 验证：缺少命令输入时不输出控制结果。
TEST(AgcControlTest, MissingCommandReturnsNullopt) {
  auto cfg = MakeBaseConfig();
  AGVC::WeightedStrategy strategy;
  AGC::ControlInput input;
  input.hasCmdRaw = false;
  input.hasMemberMeasRaw = {true, true};
  input.memberMeasRaw = {10.0, 10.0};

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  EXPECT_FALSE(out.has_value());
}

// 验证：DELTA_BASE_BASE_TAG 使用 base_tag 的缩放/偏移换算作为基准。
TEST(AgcControlTest, DeltaBaseBaseTagUsesScaledBase) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_DELTA);
  cfg.mutable_p_cmd()->set_delta_base(AGCProto::DELTA_BASE_BASE_TAG);
  cfg.mutable_p_cmd()->set_base_tag("P_BASE");
  cfg.mutable_p_cmd()->mutable_signal()->set_scale(2.0);
  cfg.mutable_p_cmd()->mutable_signal()->set_offset(5.0);

  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(3.0, {0.0, 0.0});
  input.baseRawByTag["P_BASE"] = 10.0;  // 基准值 = 10 * 2 + 5 = 25

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->desiredTotalKw, 31.0, 1e-6);
  EXPECT_NEAR(out->actualTargetKw, 31.0, 1e-6);
}

// 验证：绝对目标输入不会做逐步逼近，本轮直接按 desiredTotalKw 分配。
TEST(AgcControlTest, DirectTargetUsesDesiredTotalWithoutProgressiveStep) {
  auto cfg = MakeBaseConfig();

  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(105.0, {50.0, 50.0});

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->desiredTotalKw, 105.0, 1e-6);
  EXPECT_NEAR(out->actualTargetKw, 105.0, 1e-6);
  EXPECT_TRUE(out->hasLastDesiredTotalKw);
  EXPECT_NEAR(out->nextLastDesiredTotalKw, 105.0, 1e-6);
}

// 验证：成员 DELTA/LAST_TARGET 输出后仍会维护上一轮成员目标值缓存。
TEST(AgcControlTest, MemberDeltaBaseLastTargetKeepsMemberTargetCache) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_weight(1);
  cfg.mutable_members(1)->set_weight(1);
  cfg.mutable_members(0)->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_DELTA);
  cfg.mutable_members(0)->mutable_p_set()->set_delta_base(AGCProto::DELTA_BASE_LAST_TARGET);
  cfg.mutable_members(1)->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_DELTA);
  cfg.mutable_members(1)->mutable_p_set()->set_delta_base(AGCProto::DELTA_BASE_LAST_TARGET);

  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(40.0, {10.0, 10.0});
  input.hasLastMemberTargetKw = {true, true};
  input.lastMemberTargetKw = {15.0, 15.0};

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->hasLastMemberTargetKw.size(), 2u);
  ASSERT_EQ(out->nextLastMemberTargetKw.size(), 2u);
  EXPECT_TRUE(out->hasLastMemberTargetKw[0]);
  EXPECT_TRUE(out->hasLastMemberTargetKw[1]);
  EXPECT_NEAR(out->nextLastMemberTargetKw[0], 20.0, 1e-6);
  EXPECT_NEAR(out->nextLastMemberTargetKw[1], 20.0, 1e-6);
  EXPECT_NEAR(out->memberPublishKw[0], 5.0, 1e-6);
  EXPECT_NEAR(out->memberPublishKw[1], 5.0, 1e-6);
}

// 验证：成员设定发布值使用工程量口径，不因输出 SignalSpec 的 scale/offset 被反向换算。
TEST(AgcControlTest, MemberSetpointPublishesEngineeringValueWithoutReverseScale) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_weight(1);
  cfg.mutable_members(1)->set_weight(1);
  cfg.mutable_members(0)->mutable_p_set()->mutable_signal()->set_scale(2.0);
  cfg.mutable_members(0)->mutable_p_set()->mutable_signal()->set_offset(5.0);
  cfg.mutable_members(1)->mutable_p_set()->mutable_signal()->set_scale(2.0);
  cfg.mutable_members(1)->mutable_p_set()->mutable_signal()->set_offset(5.0);

  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(40.0, {0.0, 0.0});

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->memberPublishKw.size(), 2u);
  EXPECT_NEAR(out->memberPublishKw[0], 20.0, 1e-6);
  EXPECT_NEAR(out->memberPublishKw[1], 20.0, 1e-6);
}

// 验证：总实时输出发布工程量，不因 outputs.p_total_meas 的 scale/offset 被反向换算。
TEST(AgcControlTest, TotalMeasurementPublishesEngineeringValueWithoutReverseScale) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_outputs()->mutable_p_total_meas()->set_scale(2.0);
  cfg.mutable_outputs()->mutable_p_total_meas()->set_offset(5.0);

  auto input = MakeBaseInput(0.0, {10.0, 20.0});

  double totalMeasKw = 0.0;
  const auto publishValue = AGC::ComputeTotalMeasKw(cfg, input, &totalMeasKw);
  ASSERT_TRUE(publishValue.has_value());
  EXPECT_NEAR(totalMeasKw, 30.0, 1e-6);
  EXPECT_NEAR(*publishValue, 30.0, 1e-6);
}

// 验证：不可控成员作为被动出力，从总目标中扣除后再分配。
TEST(AgcControlTest, UncontrollableMemberIsPassiveOutput) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_controllable(false);

  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(60.0, {20.0, 0.0});

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(out->passiveKw, 20.0, 1e-6);
  EXPECT_NEAR(out->targetControllableKw, 40.0, 1e-6);
  ASSERT_EQ(out->memberTargetKw.size(), 2u);
  EXPECT_NEAR(out->memberTargetKw[1], 40.0, 1e-6);
}

// 验证：默认上下限点按当前控制口径计算，缺测的不可控成员按 0 参与动态上下限并置 BAD 质量。
TEST(AgcControlTest, DefaultPointOutputUsesZeroFallbackForMissingUncontrollableMember) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_controllable(false);
  cfg.mutable_members(1)->set_min_kw(10.0);
  cfg.mutable_members(1)->set_max_kw(80.0);

  AGC::ControlInput input;
  input.hasMemberMeasRaw = {false, false};
  input.memberMeasRaw = {0.0, 0.0};

  const auto out = AGC::ComputeDefaultPointOutput(cfg, input);
  EXPECT_NEAR(out.theoreticalLowerKw, 10.0, 1e-6);
  EXPECT_NEAR(out.theoreticalUpperKw, 80.0, 1e-6);
  EXPECT_NEAR(out.dynamicLowerKw, 10.0, 1e-6);
  EXPECT_NEAR(out.dynamicUpperKw, 80.0, 1e-6);
  EXPECT_EQ(out.dynamicQuality, DataCenterProto::QUALITY_BAD);
  EXPECT_EQ(out.uncontrollableMemberCount, 1u);
  EXPECT_EQ(out.missingUncontrollableMemberCount, 1u);
}

// 验证：不可控成员实际值就绪后，会抬高动态上下限并恢复 GOOD 质量。
TEST(AgcControlTest, DefaultPointOutputAddsMeasuredUncontrollablePowerToDynamicLimits) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_controllable(false);
  cfg.mutable_members(1)->set_min_kw(10.0);
  cfg.mutable_members(1)->set_max_kw(80.0);

  AGC::ControlInput input;
  input.hasMemberMeasRaw = {true, false};
  input.memberMeasRaw = {20.0, 0.0};

  const auto out = AGC::ComputeDefaultPointOutput(cfg, input);
  EXPECT_NEAR(out.theoreticalLowerKw, 10.0, 1e-6);
  EXPECT_NEAR(out.theoreticalUpperKw, 80.0, 1e-6);
  EXPECT_NEAR(out.dynamicLowerKw, 30.0, 1e-6);
  EXPECT_NEAR(out.dynamicUpperKw, 100.0, 1e-6);
  EXPECT_EQ(out.dynamicQuality, DataCenterProto::QUALITY_GOOD);
  EXPECT_EQ(out.uncontrollableMemberCount, 1u);
  EXPECT_EQ(out.missingUncontrollableMemberCount, 0u);
}
