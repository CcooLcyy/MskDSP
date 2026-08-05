#include "IEC61850ProtectionEngine.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <unordered_set>
#include <utility>

namespace IEC61850 {
namespace {

constexpr std::uint32_t kProtectionRetryInitialDelayMs = 5;
constexpr std::uint32_t kProtectionRetryMaxDelayMs = 1000;

bool IsNumeric(RealtimeSignalValueType type) noexcept {
  return type == RealtimeSignalValueType::INTEGER ||
         type == RealtimeSignalValueType::FLOATING;
}

bool ToRealtimeType(IEC61850Proto::PointValueType type,
                    RealtimeSignalValueType* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  switch (type) {
    case IEC61850Proto::POINT_VALUE_TYPE_BOOL:
      *output = RealtimeSignalValueType::BOOLEAN;
      return true;
    case IEC61850Proto::POINT_VALUE_TYPE_INT64:
      *output = RealtimeSignalValueType::INTEGER;
      return true;
    case IEC61850Proto::POINT_VALUE_TYPE_DOUBLE:
      *output = RealtimeSignalValueType::FLOATING;
      return true;
    default:
      return false;
  }
}

bool ToComparator(IEC61850Proto::ProtectionComparator comparator,
                  ProtectionComparator* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  switch (comparator) {
    case IEC61850Proto::PROTECTION_COMPARATOR_BOOL_TRUE:
      *output = ProtectionComparator::BOOL_TRUE;
      return true;
    case IEC61850Proto::PROTECTION_COMPARATOR_BOOL_FALSE:
      *output = ProtectionComparator::BOOL_FALSE;
      return true;
    case IEC61850Proto::PROTECTION_COMPARATOR_EQUAL:
      *output = ProtectionComparator::EQUAL;
      return true;
    case IEC61850Proto::PROTECTION_COMPARATOR_NOT_EQUAL:
      *output = ProtectionComparator::NOT_EQUAL;
      return true;
    case IEC61850Proto::PROTECTION_COMPARATOR_GREATER_THAN:
      *output = ProtectionComparator::GREATER_THAN;
      return true;
    case IEC61850Proto::PROTECTION_COMPARATOR_GREATER_OR_EQUAL:
      *output = ProtectionComparator::GREATER_OR_EQUAL;
      return true;
    case IEC61850Proto::PROTECTION_COMPARATOR_LESS_THAN:
      *output = ProtectionComparator::LESS_THAN;
      return true;
    case IEC61850Proto::PROTECTION_COMPARATOR_LESS_OR_EQUAL:
      *output = ProtectionComparator::LESS_OR_EQUAL;
      return true;
    default:
      return false;
  }
}

bool ToOutputValue(const IEC61850Proto::ProtectionOutputValue& input,
                   ProtocolRealtimeValue* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  RealtimeSignalValueType valueType;
  if (!ToRealtimeType(input.value_type(), &valueType)) {
    return false;
  }
  output->valueType = static_cast<ProtocolRealtimeValueType>(valueType);
  output->qualityBits = input.quality_bits();
  output->timestampNs = 0;
  switch (valueType) {
    case RealtimeSignalValueType::BOOLEAN:
      output->value.booleanValue = input.bool_value();
      return true;
    case RealtimeSignalValueType::INTEGER:
      output->value.integerValue = input.int_value();
      return true;
    case RealtimeSignalValueType::FLOATING:
      if (!std::isfinite(input.double_value())) {
        return false;
      }
      output->value.floatingValue = input.double_value();
      return true;
  }
  return false;
}

grpc::Status Invalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

std::uint32_t RetryDelayMs(std::uint32_t retryCount) noexcept {
  std::uint32_t delay = kProtectionRetryInitialDelayMs;
  for (std::uint32_t index = 1;
       index < retryCount && delay < kProtectionRetryMaxDelayMs; ++index) {
    if (delay > kProtectionRetryMaxDelayMs / 2) {
      delay = kProtectionRetryMaxDelayMs;
    } else {
      delay *= 2;
    }
  }
  return delay;
}

}  // namespace

grpc::Status BuildProtectionRuleConfigs(
    const IEC61850Proto::IedConfig& iedConfig,
    std::span<const ProtocolGooseSubscriptionPlan> gooseSubscriptions,
    std::span<const ProtocolSignalDefinition> realtimeSignals,
    std::vector<ProtectionRuleConfig>* rules) {
  if (rules == nullptr) {
    return Invalid("IEC61850保护规则输出参数不能为空");
  }
  rules->clear();
  rules->reserve(iedConfig.protection_rules_size());
  std::unordered_set<std::string> ruleIds;
  std::unordered_set<std::uint32_t> outputSubscriptions;
  for (int ruleIndex = 0; ruleIndex < iedConfig.protection_rules_size();
       ++ruleIndex) {
    const auto& inputRule = iedConfig.protection_rules(ruleIndex);
    if (inputRule.rule_id().empty() ||
        !ruleIds.emplace(inputRule.rule_id()).second) {
      return Invalid(std::format("IEC61850保护规则编号为空或重复: index={}",
                                 ruleIndex));
    }
    if (inputRule.conditions().empty()) {
      return Invalid(std::format("IEC61850保护规则没有输入条件: rule_id={}",
                                 inputRule.rule_id()));
    }
    std::uint32_t outputSubscriptionId = inputRule.output_subscription_id();
    if (!inputRule.output_control_ref().empty()) {
      const auto outputIt = std::find_if(
          gooseSubscriptions.begin(), gooseSubscriptions.end(),
          [&inputRule](const auto& subscription) {
            return subscription.controlRef == inputRule.output_control_ref();
          });
      if (outputIt == gooseSubscriptions.end()) {
        return Invalid(std::format(
            "IEC61850保护规则引用的GOOSE控制块不存在: rule_id={}, control_ref={}",
            inputRule.rule_id(), inputRule.output_control_ref()));
      }
      outputSubscriptionId = outputIt->subscriptionId;
    } else if (!realtimeSignals.empty()) {
      return Invalid(std::format(
          "IEC61850保护规则生产配置必须使用output_control_ref: rule_id={}",
          inputRule.rule_id()));
    }
    if (outputSubscriptionId == 0 ||
        !outputSubscriptions.emplace(outputSubscriptionId).second) {
      return Invalid(std::format(
          "IEC61850保护规则输出GOOSE订阅为空或重复: rule_id={}",
          inputRule.rule_id()));
    }
    const auto subscriptionIt = std::find_if(
        gooseSubscriptions.begin(), gooseSubscriptions.end(),
        [outputSubscriptionId](const auto& subscription) {
          return subscription.subscriptionId == outputSubscriptionId;
        });
    if (subscriptionIt == gooseSubscriptions.end()) {
      return Invalid(std::format(
          "IEC61850保护规则引用的GOOSE订阅不存在: rule_id={}, subscription_id={}",
          inputRule.rule_id(), outputSubscriptionId));
    }
    if (inputRule.assert_values_size() !=
            static_cast<int>(subscriptionIt->members.size()) ||
        inputRule.release_values_size() !=
            static_cast<int>(subscriptionIt->members.size()) ||
        subscriptionIt->members.empty()) {
      return Invalid(std::format(
          "IEC61850保护规则GOOSE输出成员数量不匹配: rule_id={}",
          inputRule.rule_id()));
    }

    ProtectionRuleConfig rule;
    rule.outputSubscriptionId = outputSubscriptionId;
    rule.assertDelayMs = inputRule.assert_delay_ms();
    rule.releaseDelayMs = inputRule.release_delay_ms();
    rule.conditions.reserve(inputRule.conditions_size());
    std::unordered_set<std::uint32_t> inputIds;
    for (const auto& inputCondition : inputRule.conditions()) {
      RealtimeSignalValueType valueType;
      ProtectionComparator comparator;
      std::uint32_t signalId = inputCondition.signal_id();
      if (!inputCondition.data_ref().empty()) {
        if (inputCondition.fc() ==
            IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED) {
          return Invalid(std::format(
              "IEC61850保护规则稳定输入引用缺少fc: rule_id={}",
              inputRule.rule_id()));
        }
        const auto signalIt = std::find_if(
            realtimeSignals.begin(), realtimeSignals.end(),
            [&inputCondition](const auto& definition) {
              return definition.dataRef == inputCondition.data_ref() &&
                     definition.fc == inputCondition.fc();
            });
        if (signalIt == realtimeSignals.end() ||
            signalIt->valueType != inputCondition.value_type()) {
          return Invalid(std::format(
              "IEC61850保护规则输入引用不存在或类型不匹配: rule_id={}, data_ref={}",
              inputRule.rule_id(), inputCondition.data_ref()));
        }
        signalId = signalIt->signalId;
      } else if (!realtimeSignals.empty()) {
        return Invalid(std::format(
            "IEC61850保护规则生产配置必须使用data_ref和fc: rule_id={}",
            inputRule.rule_id()));
      }
      if (signalId == 0 || !inputIds.emplace(signalId).second ||
          !ToRealtimeType(inputCondition.value_type(), &valueType) ||
          !ToComparator(inputCondition.comparator(), &comparator)) {
        return Invalid(std::format(
            "IEC61850保护规则输入条件参数无效: rule_id={}",
            inputRule.rule_id()));
      }
      if ((comparator == ProtectionComparator::BOOL_TRUE ||
           comparator == ProtectionComparator::BOOL_FALSE) &&
          valueType != RealtimeSignalValueType::BOOLEAN) {
        return Invalid(std::format(
            "IEC61850保护规则布尔比较器类型错误: rule_id={}",
            inputRule.rule_id()));
      }
      if ((comparator == ProtectionComparator::GREATER_THAN ||
           comparator == ProtectionComparator::GREATER_OR_EQUAL ||
           comparator == ProtectionComparator::LESS_THAN ||
           comparator == ProtectionComparator::LESS_OR_EQUAL) &&
          !IsNumeric(valueType)) {
        return Invalid(std::format(
            "IEC61850保护规则大小比较器必须使用数值类型: rule_id={}",
            inputRule.rule_id()));
      }
      ProtectionInputCondition condition;
      condition.signalId = signalId;
      condition.valueType = valueType;
      condition.comparator = comparator;
      condition.maxAgeMs = inputCondition.max_age_ms();
      switch (valueType) {
        case RealtimeSignalValueType::BOOLEAN:
          condition.expected.booleanValue = inputCondition.bool_value();
          break;
        case RealtimeSignalValueType::INTEGER:
          condition.expected.integerValue = inputCondition.int_value();
          break;
        case RealtimeSignalValueType::FLOATING:
          if (!std::isfinite(inputCondition.double_value())) {
            return Invalid(std::format(
                "IEC61850保护规则浮点比较值必须为有限值: rule_id={}",
                inputRule.rule_id()));
          }
          condition.expected.floatingValue = inputCondition.double_value();
          break;
      }
      rule.conditions.emplace_back(condition);
    }
    std::unordered_set<std::uint32_t> interlockIds;
    const auto appendInterlock = [&](std::uint32_t signalId) -> grpc::Status {
      if (signalId == 0 || !interlockIds.emplace(signalId).second ||
          inputIds.contains(signalId)) {
        return Invalid(std::format(
            "IEC61850保护规则联锁输入重复或与条件冲突: rule_id={}",
            inputRule.rule_id()));
      }
      rule.interlockSignalIds.push_back(signalId);
      return grpc::Status::OK;
    };
    for (const auto& reference : inputRule.interlock_signals()) {
      if (reference.data_ref().empty() ||
          reference.fc() ==
              IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED) {
        return Invalid(std::format("IEC61850保护规则联锁引用无效: rule_id={}",
                                   inputRule.rule_id()));
      }
      const auto signalIt = std::find_if(
          realtimeSignals.begin(), realtimeSignals.end(),
          [&reference](const auto& definition) {
            return definition.dataRef == reference.data_ref() &&
                   definition.fc == reference.fc();
          });
      if (signalIt == realtimeSignals.end() ||
          signalIt->valueType != IEC61850Proto::POINT_VALUE_TYPE_BOOL) {
        return Invalid(std::format(
            "IEC61850保护规则联锁引用不存在或不是BOOL: rule_id={}, data_ref={}",
            inputRule.rule_id(), reference.data_ref()));
      }
      const auto status = appendInterlock(signalIt->signalId);
      if (!status.ok()) {
        return status;
      }
    }
    if (!realtimeSignals.empty() && inputRule.interlock_signal_ids_size() != 0) {
      return Invalid(std::format(
          "IEC61850保护规则生产配置不能使用interlock_signal_ids: rule_id={}",
          inputRule.rule_id()));
    }
    for (const auto signalId : inputRule.interlock_signal_ids()) {
      const auto status = appendInterlock(signalId);
      if (!status.ok()) {
        return status;
      }
    }
    rule.assertValues.resize(subscriptionIt->members.size());
    rule.releaseValues.resize(subscriptionIt->members.size());
    for (std::size_t index = 0; index < subscriptionIt->members.size();
         ++index) {
      RealtimeSignalValueType expectedType;
      if (!ToRealtimeType(subscriptionIt->members[index].valueType,
                          &expectedType) ||
          !ToOutputValue(inputRule.assert_values(static_cast<int>(index)),
                         &rule.assertValues[index]) ||
          !ToOutputValue(inputRule.release_values(static_cast<int>(index)),
                         &rule.releaseValues[index]) ||
          static_cast<RealtimeSignalValueType>(
              rule.assertValues[index].valueType) != expectedType ||
          static_cast<RealtimeSignalValueType>(
              rule.releaseValues[index].valueType) != expectedType) {
        return Invalid(std::format(
            "IEC61850保护规则GOOSE输出成员类型不匹配: rule_id={}",
            inputRule.rule_id()));
      }
    }
    rules->emplace_back(std::move(rule));
  }
  return grpc::Status::OK;
}

grpc::Status BuildProtectionRuleConfigs(
    const IEC61850Proto::IedConfig& iedConfig,
    std::span<const ProtocolGooseSubscriptionPlan> gooseSubscriptions,
    std::span<const ProtocolGoosePublisherPlan> goosePublishers,
    std::span<const ProtocolSignalDefinition> realtimeSignals,
    std::vector<ProtectionRuleConfig>* rules) {
  if (goosePublishers.empty()) {
    if (!realtimeSignals.empty() && iedConfig.protection_rules_size() != 0) {
      return Invalid(
          "IEC61850生产保护规则缺少当前IED的本地GOOSE发布计划");
    }
    return BuildProtectionRuleConfigs(iedConfig, gooseSubscriptions,
                                      realtimeSignals, rules);
  }

  // 保护规则内部仍使用旧的输出字段名；这里把独立发布计划映射为同一
  // 个固定布局，实际编号保存的是publisherId，不再依赖订阅计划。
  std::vector<ProtocolGooseSubscriptionPlan> outputPlans;
  outputPlans.reserve(goosePublishers.size());
  for (const auto& publisher : goosePublishers) {
    ProtocolGooseSubscriptionPlan output;
    output.subscriptionId = publisher.publisherId;
    output.publisherIed = publisher.publisherIed;
    output.controlRef = publisher.controlRef;
    output.dataSetRef = publisher.dataSetRef;
    output.goId = publisher.goId;
    output.configRevision = publisher.configRevision;
    output.members = publisher.members;
    output.endpoints = publisher.endpoints;
    outputPlans.emplace_back(std::move(output));
  }
  return BuildProtectionRuleConfigs(
      iedConfig,
      std::span<const ProtocolGooseSubscriptionPlan>(outputPlans.data(),
                                                      outputPlans.size()),
      realtimeSignals, rules);
}

grpc::Status BuildProtectionRuleConfigs(
    const IEC61850Proto::IedConfig& iedConfig,
    std::span<const ProtocolGooseSubscriptionPlan> gooseSubscriptions,
    std::vector<ProtectionRuleConfig>* rules) {
  return BuildProtectionRuleConfigs(
      iedConfig, gooseSubscriptions,
      std::span<const ProtocolSignalDefinition>{}, rules);
}

ProtectionEngine::ProtectionEngine(std::vector<ProtectionRuleConfig> rules,
                                   std::uint64_t sessionGeneration,
                                   std::size_t actionQueueCapacity)
    : sessionGeneration_(sessionGeneration) {
  std::vector<std::uint32_t> signalIds;
  for (auto& rule : rules) {
    signalIds.reserve(signalIds.size() + rule.conditions.size() +
                      rule.interlockSignalIds.size());
    for (const auto& condition : rule.conditions) {
      signalIds.push_back(condition.signalId);
    }
    signalIds.insert(signalIds.end(), rule.interlockSignalIds.begin(),
                     rule.interlockSignalIds.end());
    rules_.push_back({.config = std::move(rule)});
  }
  std::sort(signalIds.begin(), signalIds.end());
  signalIds.erase(std::unique(signalIds.begin(), signalIds.end()),
                  signalIds.end());
  signals_.reserve(signalIds.size());
  for (const auto signalId : signalIds) {
    signals_.push_back({.signalId = signalId});
  }
  if (actionQueueCapacity == 0) {
    actionQueueCapacity = std::max<std::size_t>(1, rules_.size() * 4);
  }
  actionQueue_.resize(actionQueueCapacity);
}

bool ProtectionEngine::Compare(
    const SignalState& state,
    const ProtectionInputCondition& condition) noexcept {
  if (!state.seen || state.valueType != condition.valueType ||
      (state.qualityBits & kProtectionCommunicationInvalid) != 0) {
    return false;
  }
  switch (condition.comparator) {
    case ProtectionComparator::BOOL_TRUE:
      return state.value.booleanValue;
    case ProtectionComparator::BOOL_FALSE:
      return !state.value.booleanValue;
    case ProtectionComparator::EQUAL:
      if (condition.valueType == RealtimeSignalValueType::BOOLEAN) {
        return state.value.booleanValue == condition.expected.booleanValue;
      }
      if (condition.valueType == RealtimeSignalValueType::INTEGER) {
        return state.value.integerValue == condition.expected.integerValue;
      }
      return state.value.floatingValue == condition.expected.floatingValue;
    case ProtectionComparator::NOT_EQUAL:
      if (condition.valueType == RealtimeSignalValueType::BOOLEAN) {
        return state.value.booleanValue != condition.expected.booleanValue;
      }
      if (condition.valueType == RealtimeSignalValueType::INTEGER) {
        return state.value.integerValue != condition.expected.integerValue;
      }
      return state.value.floatingValue != condition.expected.floatingValue;
    case ProtectionComparator::GREATER_THAN:
      return condition.valueType == RealtimeSignalValueType::INTEGER
                 ? state.value.integerValue > condition.expected.integerValue
                 : state.value.floatingValue > condition.expected.floatingValue;
    case ProtectionComparator::GREATER_OR_EQUAL:
      return condition.valueType == RealtimeSignalValueType::INTEGER
                 ? state.value.integerValue >= condition.expected.integerValue
                 : state.value.floatingValue >= condition.expected.floatingValue;
    case ProtectionComparator::LESS_THAN:
      return condition.valueType == RealtimeSignalValueType::INTEGER
                 ? state.value.integerValue < condition.expected.integerValue
                 : state.value.floatingValue < condition.expected.floatingValue;
    case ProtectionComparator::LESS_OR_EQUAL:
      return condition.valueType == RealtimeSignalValueType::INTEGER
                 ? state.value.integerValue <= condition.expected.integerValue
                 : state.value.floatingValue <= condition.expected.floatingValue;
  }
  return false;
}

std::int64_t ProtectionEngine::AddDelay(std::int64_t nowNs,
                                        std::uint32_t delayMs) noexcept {
  constexpr std::int64_t kNsPerMs = 1'000'000;
  if (delayMs > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max() / kNsPerMs) ||
      nowNs > std::numeric_limits<std::int64_t>::max() -
                   static_cast<std::int64_t>(delayMs) * kNsPerMs) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return nowNs + static_cast<std::int64_t>(delayMs) * kNsPerMs;
}

std::size_t ProtectionEngine::FindSignal(std::uint32_t signalId) const noexcept {
  const auto it = std::lower_bound(
      signals_.begin(), signals_.end(), signalId,
      [](const SignalState& state, std::uint32_t value) {
        return state.signalId < value;
      });
  return it == signals_.end() || it->signalId != signalId
             ? signals_.size()
             : static_cast<std::size_t>(it - signals_.begin());
}

bool ProtectionEngine::EvaluateRule(const RuleState& rule,
                                    std::int64_t nowNs) const noexcept {
  for (const auto& condition : rule.config.conditions) {
    const auto index = FindSignal(condition.signalId);
    if (index >= signals_.size()) {
      return false;
    }
    const auto& state = signals_[index];
    const bool stale = state.timestampNs < 0 || nowNs < state.timestampNs ||
                       (condition.maxAgeMs != 0 &&
                        nowNs - state.timestampNs >
                            static_cast<std::int64_t>(condition.maxAgeMs) *
                                1'000'000);
    if (!state.seen ||
        stale ||
        !Compare(state, condition)) {
      return false;
    }
  }
  for (const auto signalId : rule.config.interlockSignalIds) {
    const auto index = FindSignal(signalId);
    if (index >= signals_.size()) {
      return false;
    }
    const auto& state = signals_[index];
    if (!state.seen || state.valueType != RealtimeSignalValueType::BOOLEAN ||
        (state.qualityBits & kProtectionCommunicationInvalid) != 0) {
      return false;
    }
    if (state.value.booleanValue) {
      return false;
    }
  }
  return true;
}

bool ProtectionEngine::QueueAction(std::size_t ruleIndex,
                                   bool asserted) noexcept {
  if (actionCount_ >= actionQueue_.size()) {
    ++statistics_.actionsDropped;
    return false;
  }
  actionQueue_[actionTail_] = {.ruleIndex = ruleIndex, .asserted = asserted};
  actionTail_ = (actionTail_ + 1) % actionQueue_.size();
  ++actionCount_;
  rules_[ruleIndex].queued = true;
  ++statistics_.actionsQueued;
  return true;
}

bool ProtectionEngine::ApplyRule(std::size_t ruleIndex, bool desired,
                                 std::int64_t nowNs) noexcept {
  auto& rule = rules_[ruleIndex];
  if (desired == rule.asserted) {
    const bool changed = rule.pending && !rule.queued && !rule.inFlight;
    rule.pending = false;
    rule.retryCount = 0;
    return changed;
  }
  if (rule.queued || rule.inFlight) {
    return false;
  }
  if (!rule.pending || rule.pendingAsserted != desired) {
    rule.pending = true;
    rule.pendingAsserted = desired;
    rule.dueNs = AddDelay(nowNs, desired ? rule.config.assertDelayMs
                                         : rule.config.releaseDelayMs);
  }
  if (nowNs < rule.dueNs || !QueueAction(ruleIndex, desired)) {
    return true;
  }
  rule.pending = false;
  return true;
}

bool ProtectionEngine::EvaluateAll(std::int64_t nowNs) noexcept {
  bool changed = false;
  for (std::size_t index = 0; index < rules_.size(); ++index) {
    const bool desired = EvaluateRule(rules_[index], nowNs);
    if (!desired && !rules_[index].config.interlockSignalIds.empty()) {
      ++statistics_.rulesBlocked;
    }
    changed = ApplyRule(index, desired, nowNs) || changed;
  }
  return changed;
}

bool ProtectionEngine::Process(const RealtimeSignalUpdate& update,
                               std::int64_t nowNs) noexcept {
  lastNowNs_ = nowNs;
  if (update.sessionGeneration != sessionGeneration_) {
    ++statistics_.sessionMismatch;
    return false;
  }
  const auto index = FindSignal(update.signalId);
  if (index >= signals_.size()) {
    ++statistics_.unknownSignal;
    return false;
  }
  auto& state = signals_[index];
  if (static_cast<RealtimeSignalValueType>(update.valueType) !=
      state.valueType && state.seen) {
    ++statistics_.typeMismatch;
    return false;
  }
  state.seen = true;
  state.valueType = update.valueType;
  state.qualityBits = update.qualityBits;
  state.timestampNs = update.timestampNs;
  state.value = update.value;
  ++statistics_.acceptedInputs;
  EvaluateAll(nowNs);
  return true;
}

bool ProtectionEngine::Tick(std::int64_t nowNs) noexcept {
  lastNowNs_ = nowNs;
  return EvaluateAll(nowNs);
}

std::size_t ProtectionEngine::DrainActions(
    std::span<ProtectionAction> actions) noexcept {
  std::size_t count = 0;
  while (count < actions.size() && actionCount_ != 0) {
    const auto queued = actionQueue_[actionHead_];
    actionHead_ = (actionHead_ + 1) % actionQueue_.size();
    --actionCount_;
    rules_[queued.ruleIndex].queued = false;
    rules_[queued.ruleIndex].inFlight = true;
    rules_[queued.ruleIndex].inFlightAsserted = queued.asserted;
    auto& output = actions[count++];
    output.ruleIndex = queued.ruleIndex;
    output.outputSubscriptionId =
        rules_[queued.ruleIndex].config.outputSubscriptionId;
    output.asserted = queued.asserted;
    const auto& values = queued.asserted
                             ? rules_[queued.ruleIndex].config.assertValues
                             : rules_[queued.ruleIndex].config.releaseValues;
    output.values = values;
  }
  return count;
}

void ProtectionEngine::CompleteAction(std::size_t ruleIndex, bool asserted,
                                      bool success) noexcept {
  if (ruleIndex >= rules_.size()) {
    return;
  }
  auto& rule = rules_[ruleIndex];
  if (!rule.inFlight || rule.inFlightAsserted != asserted) {
    return;
  }
  rule.inFlight = false;
  if (success) {
    rule.asserted = asserted;
    rule.retryCount = 0;
    return;
  }
  ++statistics_.actionSendFailures;
  rule.pending = true;
  rule.pendingAsserted = asserted;
  if (rule.retryCount != std::numeric_limits<std::uint32_t>::max()) {
    ++rule.retryCount;
  }
  rule.dueNs = AddDelay(lastNowNs_, RetryDelayMs(rule.retryCount));
}

void ProtectionEngine::Reset() noexcept {
  for (auto& state : signals_) {
    state.seen = false;
    state.qualityBits = 0;
    state.timestampNs = 0;
    state.value = {};
  }
  for (auto& rule : rules_) {
    rule.asserted = false;
    rule.pending = false;
    rule.pendingAsserted = false;
    rule.dueNs = 0;
    rule.retryCount = 0;
    rule.queued = false;
    rule.inFlight = false;
    rule.inFlightAsserted = false;
  }
  actionHead_ = 0;
  actionTail_ = 0;
  actionCount_ = 0;
  lastNowNs_ = 0;
}

std::size_t ProtectionEngine::size() const noexcept { return rules_.size(); }

std::uint64_t ProtectionEngine::sessionGeneration() const noexcept {
  return sessionGeneration_;
}

const ProtectionEngineStatistics& ProtectionEngine::statistics() const noexcept {
  return statistics_;
}

}  // namespace IEC61850
