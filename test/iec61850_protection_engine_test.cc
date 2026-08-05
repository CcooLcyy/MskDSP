#include <gtest/gtest.h>

#include <array>
#include <span>

#include "IEC61850ProtectionEngine.h"

namespace {

IEC61850::ProtocolGooseSubscriptionPlan MakeGoosePlan() {
  IEC61850::ProtocolGooseSubscriptionPlan plan;
  plan.subscriptionId = 21;
  plan.controlRef = "IED1LD0/LLN0$GO$gcb1";
  IEC61850::ProtocolGooseMemberPlan member;
  member.signalId = 200;
  member.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  plan.members.push_back(member);
  return plan;
}

IEC61850::ProtocolGoosePublisherPlan MakeGoosePublisherPlan() {
  IEC61850::ProtocolGoosePublisherPlan plan;
  plan.publisherId = 42;
  plan.controlRef = "IED1LD0/LLN0$GO$gcb1";
  IEC61850::ProtocolGooseMemberPlan member;
  member.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  plan.members.push_back(member);
  return plan;
}

IEC61850::RealtimeSignalUpdate MakeInput(bool value, std::int64_t timestampNs,
                                         std::uint64_t generation = 9) {
  IEC61850::RealtimeSignalUpdate update;
  update.signalId = 100;
  update.sessionGeneration = generation;
  update.valueType = IEC61850::RealtimeSignalValueType::BOOLEAN;
  update.timestampNs = timestampNs;
  update.value.booleanValue = value;
  return update;
}

IEC61850Proto::ProtectionRule MakeRule() {
  IEC61850Proto::ProtectionRule rule;
  rule.set_rule_id("trip-rule");
  auto* condition = rule.add_conditions();
  condition->set_signal_id(100);
  condition->set_comparator(
      IEC61850Proto::PROTECTION_COMPARATOR_BOOL_TRUE);
  condition->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule.set_output_subscription_id(21);
  auto* asserted = rule.add_assert_values();
  asserted->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  asserted->set_bool_value(true);
  auto* released = rule.add_release_values();
  released->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  released->set_bool_value(false);
  return rule;
}

// 验证：布尔保护条件满足后按动作延时只生成一次GOOSE动作。
TEST(IEC61850ProtectionEngineTest, DelaysAndQueuesAssertAction) {
  IEC61850Proto::IedConfig ied;
  *ied.add_protection_rules() = MakeRule();
  const auto goosePlan = MakeGoosePlan();
  std::vector<IEC61850::ProtectionRuleConfig> rules;
  ASSERT_TRUE(IEC61850::BuildProtectionRuleConfigs(
                  ied, std::span<const IEC61850::ProtocolGooseSubscriptionPlan>(
                           &goosePlan, 1),
                  &rules)
                  .ok());
  ASSERT_EQ(rules.size(), 1u);
  rules[0].assertDelayMs = 5;
  IEC61850::ProtectionEngine engine(std::move(rules), 9);

  ASSERT_TRUE(engine.Process(MakeInput(true, 1'000'000), 1'000'000));
  EXPECT_EQ(engine.DrainActions(std::span<IEC61850::ProtectionAction>{}), 0u);
  EXPECT_TRUE(engine.Tick(5'999'999));
  std::array<IEC61850::ProtectionAction, 2> actions;
  EXPECT_EQ(engine.DrainActions(actions), 0u);
  EXPECT_TRUE(engine.Tick(6'000'000));
  ASSERT_EQ(engine.DrainActions(actions), 1u);
  EXPECT_EQ(actions[0].outputSubscriptionId, 21u);
  EXPECT_TRUE(actions[0].asserted);
  ASSERT_EQ(actions[0].values.size(), 1u);
  EXPECT_TRUE(actions[0].values[0].value.booleanValue);
  engine.CompleteAction(actions[0].ruleIndex, actions[0].asserted, true);
}

// 验证：联锁有效或输入失效时规则不动作，并在已动作后产生释放值。
TEST(IEC61850ProtectionEngineTest, InterlockAndInvalidQualityReleaseAction) {
  IEC61850Proto::IedConfig ied;
  auto* rule = ied.add_protection_rules();
  *rule = MakeRule();
  rule->add_interlock_signal_ids(101);
  const auto goosePlan = MakeGoosePlan();
  std::vector<IEC61850::ProtectionRuleConfig> rules;
  ASSERT_TRUE(IEC61850::BuildProtectionRuleConfigs(
                  ied, std::span<const IEC61850::ProtocolGooseSubscriptionPlan>(
                           &goosePlan, 1),
                  &rules)
                  .ok());
  IEC61850::ProtectionEngine engine(std::move(rules), 9);

  auto interlock = MakeInput(false, 1'000'000);
  interlock.signalId = 101;
  ASSERT_TRUE(engine.Process(interlock, 1'000'000));
  ASSERT_TRUE(engine.Process(MakeInput(true, 1'000'000), 1'000'000));
  std::array<IEC61850::ProtectionAction, 2> actions;
  ASSERT_EQ(engine.DrainActions(actions), 1u);
  EXPECT_TRUE(actions[0].asserted);
  engine.CompleteAction(actions[0].ruleIndex, actions[0].asserted, true);

  interlock.value.booleanValue = true;
  ASSERT_TRUE(engine.Process(interlock, 2'000'000));
  ASSERT_EQ(engine.DrainActions(actions), 1u);
  EXPECT_FALSE(actions[0].asserted);
  engine.CompleteAction(actions[0].ruleIndex, actions[0].asserted, true);

  interlock.value.booleanValue = false;
  interlock.qualityBits = 1u << 31;
  ASSERT_TRUE(engine.Process(interlock, 3'000'000));
  EXPECT_EQ(engine.DrainActions(actions), 0u);
}

// 验证：输出数据集数量或类型不匹配时拒绝保护规则，避免发送错误GOOSE。
TEST(IEC61850ProtectionEngineTest, RejectsMismatchedGooseOutput) {
  IEC61850Proto::IedConfig ied;
  auto* rule = ied.add_protection_rules();
  *rule = MakeRule();
  rule->mutable_assert_values(0)->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_INT64);
  const auto goosePlan = MakeGoosePlan();
  std::vector<IEC61850::ProtectionRuleConfig> rules;
  const auto status = IEC61850::BuildProtectionRuleConfigs(
      ied, std::span<const IEC61850::ProtocolGooseSubscriptionPlan>(
               &goosePlan, 1),
      &rules);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(rules.empty());
}

// 验证：旧会话代际的输入不能触发保护动作。
TEST(IEC61850ProtectionEngineTest, RejectsStaleSessionInput) {
  IEC61850Proto::IedConfig ied;
  *ied.add_protection_rules() = MakeRule();
  const auto goosePlan = MakeGoosePlan();
  std::vector<IEC61850::ProtectionRuleConfig> rules;
  ASSERT_TRUE(IEC61850::BuildProtectionRuleConfigs(
                  ied, std::span<const IEC61850::ProtocolGooseSubscriptionPlan>(
                           &goosePlan, 1),
                  &rules)
                  .ok());
  IEC61850::ProtectionEngine engine(std::move(rules), 9);
  EXPECT_FALSE(engine.Process(MakeInput(true, 1'000'000, 8), 1'000'000));
  std::array<IEC61850::ProtectionAction, 1> actions;
  EXPECT_EQ(engine.DrainActions(actions), 0u);
  EXPECT_EQ(engine.statistics().sessionMismatch, 1u);
}

// 验证：生产规则使用稳定data_ref/fc和control_ref时能解析为运行时编号。
TEST(IEC61850ProtectionEngineTest, ResolvesStableReferences) {
  IEC61850Proto::IedConfig ied;
  auto* rule = ied.add_protection_rules();
  rule->set_rule_id("stable-rule");
  auto* condition = rule->add_conditions();
  condition->set_data_ref("IED1LD0/XSWI1.Pos.stVal");
  condition->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  condition->set_comparator(
      IEC61850Proto::PROTECTION_COMPARATOR_BOOL_TRUE);
  condition->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->set_output_control_ref("IED1LD0/LLN0$GO$gcb1");
  rule->add_assert_values()->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->add_release_values()->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_BOOL);

  const auto goosePlan = MakeGoosePlan();
  const auto publisher = MakeGoosePublisherPlan();
  IEC61850::ProtocolSignalDefinition signal;
  signal.signalId = 101;
  signal.dataRef = "IED1LD0/XSWI1.Pos.stVal";
  signal.fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST;
  signal.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  std::vector<IEC61850::ProtectionRuleConfig> rules;
  ASSERT_TRUE(IEC61850::BuildProtectionRuleConfigs(
                  ied, std::span<const IEC61850::ProtocolGooseSubscriptionPlan>(
                           &goosePlan, 1),
                  std::span<const IEC61850::ProtocolGoosePublisherPlan>(
                      &publisher, 1),
                  std::span<const IEC61850::ProtocolSignalDefinition>(&signal,
                                                                        1),
                  &rules)
                  .ok());
  ASSERT_EQ(rules.size(), 1u);
  ASSERT_EQ(rules[0].conditions.size(), 1u);
  EXPECT_EQ(rules[0].conditions[0].signalId, 101u);
  EXPECT_EQ(rules[0].outputSubscriptionId, 42u);
}

// 验证生产保护规则没有本地GOOSE发布计划时拒绝启动，不能把远端订阅当作输出。
TEST(IEC61850ProtectionEngineTest,
     RejectsProductionRuleWithoutLocalGoosePublisher) {
  IEC61850Proto::IedConfig ied;
  auto* rule = ied.add_protection_rules();
  rule->set_rule_id("missing-publisher-rule");
  auto* condition = rule->add_conditions();
  condition->set_data_ref("IED1LD0/XSWI1.Pos.stVal");
  condition->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  condition->set_comparator(
      IEC61850Proto::PROTECTION_COMPARATOR_BOOL_TRUE);
  condition->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->set_output_control_ref("IED1LD0/LLN0$GO$gcb1");
  rule->add_assert_values()->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->add_release_values()->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_BOOL);

  const auto subscription = MakeGoosePlan();
  IEC61850::ProtocolSignalDefinition signal;
  signal.signalId = 101;
  signal.dataRef = "IED1LD0/XSWI1.Pos.stVal";
  signal.fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST;
  signal.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  std::vector<IEC61850::ProtectionRuleConfig> rules;
  const auto status = IEC61850::BuildProtectionRuleConfigs(
      ied,
      std::span<const IEC61850::ProtocolGooseSubscriptionPlan>(&subscription,
                                                                1),
      std::span<const IEC61850::ProtocolGoosePublisherPlan>{},
      std::span<const IEC61850::ProtocolSignalDefinition>(&signal, 1),
      &rules);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(rules.empty());
}

// 验证保护规则优先解析独立GOOSE发布计划，并保存发布端编号。
TEST(IEC61850ProtectionEngineTest, ResolvesIndependentGoosePublisher) {
  IEC61850Proto::IedConfig ied;
  auto* rule = ied.add_protection_rules();
  rule->set_rule_id("publisher-rule");
  auto* condition = rule->add_conditions();
  condition->set_signal_id(100);
  condition->set_comparator(
      IEC61850Proto::PROTECTION_COMPARATOR_BOOL_TRUE);
  condition->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->set_output_control_ref("IED1LD0/LLN0$GO$gcb1");
  rule->add_assert_values()->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->add_release_values()->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_BOOL);

  const auto publisher = MakeGoosePublisherPlan();
  std::vector<IEC61850::ProtectionRuleConfig> rules;
  ASSERT_TRUE(IEC61850::BuildProtectionRuleConfigs(
                  ied,
                  std::span<const IEC61850::ProtocolGooseSubscriptionPlan>{},
                  std::span<const IEC61850::ProtocolGoosePublisherPlan>(
                      &publisher, 1),
                  std::span<const IEC61850::ProtocolSignalDefinition>{},
                  &rules)
                  .ok());
  ASSERT_EQ(rules.size(), 1u);
  EXPECT_EQ(rules.front().outputSubscriptionId, 42u);
}

// 验证：发送侧失败不会确认动作，后续按有界退避重新排队同一动作。
TEST(IEC61850ProtectionEngineTest, RetriesFailedAction) {
  IEC61850Proto::IedConfig ied;
  *ied.add_protection_rules() = MakeRule();
  const auto goosePlan = MakeGoosePlan();
  std::vector<IEC61850::ProtectionRuleConfig> rules;
  ASSERT_TRUE(IEC61850::BuildProtectionRuleConfigs(
                  ied, std::span<const IEC61850::ProtocolGooseSubscriptionPlan>(
                           &goosePlan, 1),
                  &rules)
                  .ok());
  IEC61850::ProtectionEngine engine(std::move(rules), 9);
  ASSERT_TRUE(engine.Process(MakeInput(true, 1'000'000), 1'000'000));
  std::array<IEC61850::ProtectionAction, 1> actions;
  ASSERT_EQ(engine.DrainActions(actions), 1u);
  engine.CompleteAction(actions[0].ruleIndex, actions[0].asserted, false);
  EXPECT_EQ(engine.statistics().actionSendFailures, 1u);
  EXPECT_TRUE(engine.Tick(1'000'000 + 4'000'000));
  EXPECT_EQ(engine.DrainActions(actions), 0u);
  ASSERT_TRUE(engine.Tick(1'000'000 + 6'000'000));
  ASSERT_EQ(engine.DrainActions(actions), 1u);
  EXPECT_TRUE(actions[0].asserted);
}

}  // namespace
