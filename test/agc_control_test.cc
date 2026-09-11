#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "AGCControl.h"
#include "AGCGroupValidation.h"

namespace {

mskdsp::numeric::Decimal20 Decimal(std::string_view text) {
  auto value = mskdsp::numeric::Decimal20::Parse(text);
  EXPECT_TRUE(value.has_value());
  return value.value_or(mskdsp::numeric::Decimal20{});
}

mskdsp::numeric::Decimal20 Decimal(double value) {
  auto decimal = mskdsp::numeric::Decimal20::FromDouble(value);
  EXPECT_TRUE(decimal.has_value());
  return decimal.value_or(mskdsp::numeric::Decimal20{});
}

double Legacy(const mskdsp::numeric::Decimal20 &value) {
  return value.ToDouble().value_or(0.0);
}

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
  input.cmdRaw = Decimal(cmdRaw);
  input.hasMemberMeasRaw.assign(measRaw.size(), true);
  input.memberMeasRaw.reserve(measRaw.size());
  for (const auto value : measRaw) {
    input.memberMeasRaw.emplace_back(Decimal(value));
  }
  return input;
}
}  // 命名空间结束

// 验证：AGC 从十进制文本读取倍率、偏移、容量和权重，并在真实控制计算中保持 20 位小数语义。
TEST(AgcControlTest, DecimalConfigurationAndInputsRemainExactThroughAllocation) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_p_cmd()->mutable_signal()->set_scale_decimal("0.1");
  cfg.mutable_p_cmd()->mutable_signal()->set_offset_decimal("0.2");
  cfg.mutable_members(0)->set_capacity_kw_decimal("1");
  cfg.mutable_members(0)->set_weight_decimal("1");
  cfg.mutable_members(0)->mutable_p_meas()->set_scale_decimal("0.1");
  cfg.mutable_members(1)->set_capacity_kw_decimal("1");
  cfg.mutable_members(1)->set_weight_decimal("2");
  cfg.mutable_members(1)->mutable_p_meas()->set_scale_decimal("0.1");

  AGC::ControlInput input;
  input.hasCmdRaw = true;
  input.cmdRaw = Decimal("1");
  input.hasMemberMeasRaw = {true, true};
  input.memberMeasRaw = {Decimal("1"), Decimal("2")};

  AGVC::WeightedStrategy strategy;
  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->totalMeasKw.ToFixedString(), "0.30000000000000000000");
  EXPECT_EQ(out->desiredTotalKw.ToFixedString(), "0.30000000000000000000");
  ASSERT_EQ(out->memberTargetKw.size(), 2u);
  EXPECT_EQ(out->memberTargetKw[0].ToFixedString(), "0.10000000000000000000");
  EXPECT_EQ(out->memberTargetKw[1].ToFixedString(), "0.20000000000000000000");
  EXPECT_TRUE(out->unallocatedKw.IsZero());
}

// 验证：AGC 的 PI 参数、积分记忆和偏置优先使用十进制文本，计算结果不经过 double 中转。
TEST(AgcControlTest, DecimalPiProfileRemainsExact) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_weight_decimal("1");
  cfg.mutable_members(1)->set_weight_decimal("1");
  AGC::ControlInput input;
  input.hasCmdRaw = true;
  input.cmdRaw = Decimal("0.2");
  input.hasMemberMeasRaw = {true, true};
  input.memberMeasRaw = {Decimal("0"), Decimal("0.1")};
  auto *profile = input.controlProfile.add_members();
  profile->set_member_name("m1");
  profile->set_up_p_gain_decimal("0.1");
  profile->set_up_i_gain_decimal("0.1");
  profile->set_up_bias_kw_decimal("0.01");
  profile->set_integral_limit_kw_decimal("1");

  AGVC::WeightedStrategy strategy;
  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->nextIntegralMemoryKw[0].ToFixedString(), "0.10000000000000000000");
  EXPECT_EQ(out->memberTargetKw[0].ToFixedString(), "0.13000000000000000000");
}

// 验证：非空但非法的十进制配置不会回退旧 double，而是被严格拒绝。
TEST(AgcControlTest, InvalidDecimalConfigurationDoesNotFallBackToLegacyDouble) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_capacity_kw_decimal("1.2.3");
  EXPECT_FALSE(AGC::ValidateGroupConfig(cfg).ok());

  cfg.mutable_members(0)->set_capacity_kw_decimal("100");
  cfg.mutable_p_cmd()->mutable_signal()->set_scale_decimal("nan");
  EXPECT_FALSE(AGC::ValidateGroupConfig(cfg).ok());
}

// 验证：成员最小出力不能超过由 capacity_kw 和 max_kw 共同确定的有效上限。
TEST(AgcControlTest, MinimumAboveEffectiveCapacityLimitIsRejected) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_capacity_kw_decimal("100");
  cfg.mutable_members(0)->set_min_kw_decimal("100.00000000000000000001");
  cfg.mutable_members(0)->set_max_kw_decimal("0");

  EXPECT_FALSE(AGC::ValidateGroupConfig(cfg).ok());

  cfg.mutable_members(0)->set_min_kw_decimal("150");
  cfg.mutable_members(0)->set_max_kw_decimal("200");
  EXPECT_FALSE(AGC::ValidateGroupConfig(cfg).ok());
}

// 验证：可控成员的有效分配权重之和溢出 Decimal20 时在配置阶段拒绝。
TEST(AgcControlTest, AllocationWeightSumOverflowIsRejected) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_weight_decimal("9e99");
  cfg.mutable_members(1)->set_weight_decimal("9e99");

  EXPECT_FALSE(AGC::ValidateGroupConfig(cfg).ok());
}

// 验证：绝对总设定按权重分配，并输出派生点与成员设定。
TEST(AgcControlTest, AbsoluteCommandAllocatesByWeight) {
  auto cfg = MakeBaseConfig();
  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(60.0, {10.0, 20.0});

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(Legacy(out->totalMeasKw), 30.0, 1e-6);
  EXPECT_NEAR(Legacy(out->desiredTotalKw), 60.0, 1e-6);
  EXPECT_NEAR(Legacy(out->actualTargetKw), 60.0, 1e-6);
  ASSERT_EQ(out->memberTargetKw.size(), 2u);
  EXPECT_NEAR(Legacy(out->memberTargetKw[0]), 20.0, 1e-6);
  EXPECT_NEAR(Legacy(out->memberTargetKw[1]), 40.0, 1e-6);
  EXPECT_TRUE(out->publishTotalMeas);
  EXPECT_TRUE(out->publishTotalTarget);
  EXPECT_TRUE(out->publishTotalError);
  ASSERT_EQ(out->memberPublishKw.size(), 2u);
  EXPECT_NEAR(Legacy(out->memberPublishKw[0]), 20.0, 1e-6);
  EXPECT_NEAR(Legacy(out->memberPublishKw[1]), 40.0, 1e-6);
  EXPECT_TRUE(out->hasLastDesiredTotalKw);
  EXPECT_NEAR(Legacy(out->nextLastDesiredTotalKw), 60.0, 1e-6);
}

// 验证：固定参数按成员独立生效，积分记忆在控制计算后更新。
TEST(AgcControlTest, MemberControlProfileAppliesIndependentPiCorrection) {
  auto cfg = MakeBaseConfig();
  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(60.0, {10.0, 20.0});
  auto *profile = input.controlProfile.mutable_members()->Add();
  profile->set_member_name("m1");
  profile->set_up_p_gain(0.5);
  profile->set_up_i_gain(0.1);
  profile->set_integral_limit_kw(100.0);
  input.controlPeriodSeconds = 1.0;

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(Legacy(out->memberTargetKw[0]), 26.0, 1e-6);
  EXPECT_NEAR(Legacy(out->memberTargetKw[1]), 40.0, 1e-6);
  EXPECT_NEAR(Legacy(out->nextIntegralMemoryKw[0]), 10.0, 1e-6);
}

// 验证：负功率成员经 PI 修正后仍受负数 max_kw 上限约束。
TEST(AgcControlTest, NegativeMemberMaximumStillClampsPiCorrection) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_min_kw_decimal("-100");
  cfg.mutable_members(0)->set_max_kw_decimal("-10");
  cfg.mutable_members(1)->set_controllable(false);

  AGC::ControlInput input;
  input.hasCmdRaw = true;
  input.cmdRaw = Decimal("-50");
  input.hasMemberMeasRaw = {true, true};
  input.memberMeasRaw = {Decimal("-100"), Decimal("0")};
  auto *profile = input.controlProfile.add_members();
  profile->set_member_name("m1");
  profile->set_up_p_gain_decimal("1");

  AGVC::WeightedStrategy strategy;
  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->memberTargetKw[0].ToFixedString(),
            "-10.00000000000000000000");
}

// 验证：周期直分配模式完全跳过 PI 比例/积分修正，仅按基础策略分配。
TEST(AgcControlTest, DirectCyclicModeDisablesPiCorrection) {
  auto cfg = MakeBaseConfig();
  cfg.set_control_mode(AGCProto::CONTROL_MODE_DIRECT_CYCLIC);
  cfg.set_calculation_execution_period_seconds(1.0);
  cfg.set_command_control_period_seconds(4.0);
  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(60.0, {10.0, 20.0});
  input.enablePi = false;
  auto *profile = input.controlProfile.mutable_members()->Add();
  profile->set_member_name("m1");
  profile->set_up_p_gain(10.0);
  profile->set_up_i_gain(10.0);
  input.integralMemoryKw = {Decimal("100"), Decimal("0")};

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(Legacy(out->memberTargetKw[0]), 20.0, 1e-6);
  EXPECT_NEAR(Legacy(out->memberTargetKw[1]), 40.0, 1e-6);
  EXPECT_NEAR(Legacy(out->nextIntegralMemoryKw[0]), 100.0, 1e-6);
}

// 验证：周期直分配模式强制校验计算周期和命令周期范围。
TEST(AgcControlTest, DirectCyclicModeValidatesPeriods) {
  auto cfg = MakeBaseConfig();
  cfg.set_control_mode(AGCProto::CONTROL_MODE_DIRECT_CYCLIC);
  cfg.set_calculation_execution_period_seconds(0.5);
  cfg.set_command_control_period_seconds(4.0);
  EXPECT_FALSE(AGC::ValidateGroupConfig(cfg).ok());
  cfg.set_calculation_execution_period_seconds(1.0);
  cfg.set_command_control_period_seconds(31.0);
  EXPECT_FALSE(AGC::ValidateGroupConfig(cfg).ok());
  cfg.set_command_control_period_seconds(4.0);
  EXPECT_TRUE(AGC::ValidateGroupConfig(cfg).ok());
}

// 验证：调试目标覆盖临时总目标，但不改变原有成员分配策略。
TEST(AgcControlTest, DesiredTotalOverrideUsesTemporaryTuningTarget) {
  auto cfg = MakeBaseConfig();
  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(10.0, {0.0, 0.0});
  input.hasDesiredTotalOverride = true;
  input.desiredTotalOverrideKw = Decimal("80");

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(Legacy(out->desiredTotalKw), 80.0, 1e-6);
  EXPECT_NEAR(Legacy(out->memberTargetKw[0]), 80.0 / 3.0, 1e-6);
  EXPECT_NEAR(Legacy(out->memberTargetKw[1]), 160.0 / 3.0, 1e-6);
}

// 验证：DELTA_BASE_LAST_TARGET 使用上一轮期望总目标值作为基准。
TEST(AgcControlTest, DeltaBaseLastTargetUsesDesiredTotal) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_DELTA);
  cfg.mutable_p_cmd()->set_delta_base(AGCProto::DELTA_BASE_LAST_TARGET);

  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(5.0, {10.0, 10.0});
  input.hasLastDesiredTotalKw = true;
  input.lastDesiredTotalKw = Decimal("50");

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(Legacy(out->desiredTotalKw), 55.0, 1e-6);
  EXPECT_NEAR(Legacy(out->actualTargetKw), 55.0, 1e-6);
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
  input.lastMemberTargetKw = {Decimal("15"), Decimal("15")};

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->memberPublishKw.size(), 2u);
  EXPECT_NEAR(Legacy(out->memberPublishKw[0]), 5.0, 1e-6);
  EXPECT_NEAR(Legacy(out->memberPublishKw[1]), 5.0, 1e-6);
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
  EXPECT_GT(out->unallocatedKw, Decimal("0"));
  EXPECT_NEAR(Legacy(out->actualTargetKw), 20.0, 1e-6);
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
  EXPECT_NEAR(Legacy(out->memberTargetKw[0]), 10.0, 1e-6);
  EXPECT_NEAR(Legacy(out->memberTargetKw[1]), 20.0, 1e-6);
  EXPECT_LT(out->unallocatedKw, Decimal("0"));
  EXPECT_NEAR(Legacy(out->actualTargetKw), 30.0, 1e-6);
}

// 验证：缺少命令输入时不输出控制结果。
TEST(AgcControlTest, MissingCommandReturnsNullopt) {
  auto cfg = MakeBaseConfig();
  AGVC::WeightedStrategy strategy;
  AGC::ControlInput input;
  input.hasCmdRaw = false;
  input.hasMemberMeasRaw = {true, true};
  input.memberMeasRaw = {Decimal("10"), Decimal("10")};

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
  input.baseRawByTag["P_BASE"] = Decimal("10");  // 基准值 = 10 * 2 + 5 = 25

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(Legacy(out->desiredTotalKw), 31.0, 1e-6);
  EXPECT_NEAR(Legacy(out->actualTargetKw), 31.0, 1e-6);
}

// 验证：绝对目标输入不会做逐步逼近，本轮直接按 desiredTotalKw 分配。
TEST(AgcControlTest, DirectTargetUsesDesiredTotalWithoutProgressiveStep) {
  auto cfg = MakeBaseConfig();

  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(105.0, {50.0, 50.0});

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(Legacy(out->desiredTotalKw), 105.0, 1e-6);
  EXPECT_NEAR(Legacy(out->actualTargetKw), 105.0, 1e-6);
  EXPECT_TRUE(out->hasLastDesiredTotalKw);
  EXPECT_NEAR(Legacy(out->nextLastDesiredTotalKw), 105.0, 1e-6);
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
  input.lastMemberTargetKw = {Decimal("15"), Decimal("15")};

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->hasLastMemberTargetKw.size(), 2u);
  ASSERT_EQ(out->nextLastMemberTargetKw.size(), 2u);
  EXPECT_TRUE(out->hasLastMemberTargetKw[0]);
  EXPECT_TRUE(out->hasLastMemberTargetKw[1]);
  EXPECT_NEAR(Legacy(out->nextLastMemberTargetKw[0]), 20.0, 1e-6);
  EXPECT_NEAR(Legacy(out->nextLastMemberTargetKw[1]), 20.0, 1e-6);
  EXPECT_NEAR(Legacy(out->memberPublishKw[0]), 5.0, 1e-6);
  EXPECT_NEAR(Legacy(out->memberPublishKw[1]), 5.0, 1e-6);
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
  EXPECT_NEAR(Legacy(out->memberPublishKw[0]), 20.0, 1e-6);
  EXPECT_NEAR(Legacy(out->memberPublishKw[1]), 20.0, 1e-6);
}

// 验证：总实时输出发布工程量，不因 outputs.p_total_meas 的 scale/offset 被反向换算。
TEST(AgcControlTest, TotalMeasurementPublishesEngineeringValueWithoutReverseScale) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_outputs()->mutable_p_total_meas()->set_scale(2.0);
  cfg.mutable_outputs()->mutable_p_total_meas()->set_offset(5.0);

  auto input = MakeBaseInput(0.0, {10.0, 20.0});

  auto totalMeasKw = Decimal("0");
  const auto publishValue = AGC::ComputeTotalMeasKw(cfg, input, &totalMeasKw);
  ASSERT_TRUE(publishValue.has_value());
  EXPECT_NEAR(Legacy(totalMeasKw), 30.0, 1e-6);
  EXPECT_NEAR(Legacy(*publishValue), 30.0, 1e-6);
}

// 验证：不可控成员作为被动出力，从总目标中扣除后再分配。
TEST(AgcControlTest, UncontrollableMemberIsPassiveOutput) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_controllable(false);

  AGVC::WeightedStrategy strategy;
  auto input = MakeBaseInput(60.0, {20.0, 0.0});

  auto out = AGC::ComputeControlOutput(cfg, input, strategy);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(Legacy(out->passiveKw), 20.0, 1e-6);
  EXPECT_NEAR(Legacy(out->targetControllableKw), 40.0, 1e-6);
  ASSERT_EQ(out->memberTargetKw.size(), 2u);
  EXPECT_NEAR(Legacy(out->memberTargetKw[1]), 40.0, 1e-6);
}

// 验证：默认上下限点按当前控制口径计算，缺测的不可控成员按 0 参与动态上下限并置 BAD 质量。
TEST(AgcControlTest, DefaultPointOutputUsesZeroFallbackForMissingUncontrollableMember) {
  auto cfg = MakeBaseConfig();
  cfg.mutable_members(0)->set_controllable(false);
  cfg.mutable_members(1)->set_min_kw(10.0);
  cfg.mutable_members(1)->set_max_kw(80.0);

  AGC::ControlInput input;
  input.hasMemberMeasRaw = {false, false};
  input.memberMeasRaw = {Decimal("0"), Decimal("0")};

  const auto out = AGC::ComputeDefaultPointOutput(cfg, input);
  EXPECT_NEAR(Legacy(out.theoreticalLowerKw), 10.0, 1e-6);
  EXPECT_NEAR(Legacy(out.theoreticalUpperKw), 80.0, 1e-6);
  EXPECT_NEAR(Legacy(out.dynamicLowerKw), 10.0, 1e-6);
  EXPECT_NEAR(Legacy(out.dynamicUpperKw), 80.0, 1e-6);
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
  input.memberMeasRaw = {Decimal("20"), Decimal("0")};

  const auto out = AGC::ComputeDefaultPointOutput(cfg, input);
  EXPECT_NEAR(Legacy(out.theoreticalLowerKw), 10.0, 1e-6);
  EXPECT_NEAR(Legacy(out.theoreticalUpperKw), 80.0, 1e-6);
  EXPECT_NEAR(Legacy(out.dynamicLowerKw), 30.0, 1e-6);
  EXPECT_NEAR(Legacy(out.dynamicUpperKw), 100.0, 1e-6);
  EXPECT_EQ(out.dynamicQuality, DataCenterProto::QUALITY_GOOD);
  EXPECT_EQ(out.uncontrollableMemberCount, 1u);
  EXPECT_EQ(out.missingUncontrollableMemberCount, 0u);
}
