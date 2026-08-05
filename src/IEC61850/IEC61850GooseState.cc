#include "IEC61850GooseState.h"

#include <limits>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace IEC61850 {
namespace {

bool IsNonEmptyIdentity(const GooseSubscriptionConfig& config) {
  return !config.appIds.empty() && !config.gocbRef.empty() &&
         !config.dataSetRef.empty() && !config.goId.empty();
}

bool ValueMatchesType(const MmsValue& value, GooseValueType type) {
  switch (type) {
    case GooseValueType::BOOLEAN:
      return std::holds_alternative<bool>(value);
    case GooseValueType::INTEGER:
      return std::holds_alternative<std::int64_t>(value);
    case GooseValueType::FLOATING:
      return std::holds_alternative<double>(value);
    case GooseValueType::STRING:
      return std::holds_alternative<std::string>(value);
    case GooseValueType::BYTES:
      return std::holds_alternative<std::vector<std::uint8_t>>(value);
  }
  return false;
}

bool PayloadEqual(const MmsDataValue& left, const MmsDataValue& right) {
  return left.dataRef == right.dataRef && left.fc == right.fc &&
         left.value == right.value;
}

}  // namespace

GooseStateMachine::GooseStateMachine(GooseSubscriptionConfig config) :
  config_(std::move(config)), configValid_(IsNonEmptyIdentity(config_)) {
  std::unordered_set<std::string> memberRefs;
  std::unordered_set<std::uint16_t> appIds;
  for (const auto appId : config_.appIds) {
    if (appId == 0 || !appIds.emplace(appId).second) {
      configValid_ = false;
      break;
    }
  }
  for (const auto& member : config_.members) {
    if (member.dataRef.empty() ||
        member.fc == IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED ||
        !memberRefs.emplace(member.dataRef + "#" +
                            std::to_string(static_cast<int>(member.fc)))
             .second) {
      configValid_ = false;
      break;
    }
  }
}

bool GooseStateMachine::MatchesSubscription(
    const GooseMessage& message) const {
  return configValid_ &&
         std::ranges::find(config_.appIds, message.appId) !=
             config_.appIds.end() &&
         config_.gocbRef == message.gocbRef &&
         config_.dataSetRef == message.dataSetRef &&
         config_.goId == message.goId &&
         config_.configRevision == message.configRevision;
}

bool GooseStateMachine::MatchesMembers(const GooseMessage& message) const {
  if (message.values.size() != config_.members.size()) {
    return config_.members.empty() && message.values.empty();
  }
  for (std::size_t index = 0; index < config_.members.size(); ++index) {
    const auto& expected = config_.members[index];
    const auto& actual = message.values[index];
    if (actual.dataRef != expected.dataRef || actual.fc != expected.fc ||
        !ValueMatchesType(actual.value, expected.valueType)) {
      return false;
    }
  }
  return true;
}

bool GooseStateMachine::IsNewerSequence(const GooseMessage& message) const {
  if (state_ == GooseInputState::NOT_RECEIVED) {
    return message.stateNumber > 0;
  }
  if (message.stateNumber < stateNumber_) {
    return false;
  }
  if (message.stateNumber == stateNumber_) {
    return sequenceNumber_ != std::numeric_limits<std::uint32_t>::max() &&
           message.sequenceNumber == sequenceNumber_ + 1;
  }
  return message.sequenceNumber == 0;
}

GooseProcessResult GooseStateMachine::Process(const GooseMessage& message,
                                             std::int64_t nowMs) {
  if (!MatchesSubscription(message) || message.needsCommissioning ||
      (message.simulation && !config_.allowSimulation) ||
      message.timeAllowedToLiveMs == 0 || !MatchesMembers(message)) {
    return GooseProcessResult::REJECTED;
  }
  if (state_ != GooseInputState::NOT_RECEIVED &&
      message.stateNumber == stateNumber_ &&
      message.sequenceNumber == sequenceNumber_) {
    if (message.values.size() != values_.size()) {
      return GooseProcessResult::CONFLICT;
    }
    for (std::size_t index = 0; index < values_.size(); ++index) {
      if (!PayloadEqual(message.values[index], values_[index])) {
        return GooseProcessResult::CONFLICT;
      }
    }
    const bool recovered = state_ == GooseInputState::TIMED_OUT;
    state_ = GooseInputState::ACTIVE;
    if (nowMs > std::numeric_limits<std::int64_t>::max() -
                    message.timeAllowedToLiveMs) {
      expiresAtMs_ = std::numeric_limits<std::int64_t>::max();
    } else {
      expiresAtMs_ = nowMs + message.timeAllowedToLiveMs;
    }
    return recovered ? GooseProcessResult::RECOVERED
                     : GooseProcessResult::DUPLICATE;
  }
  if (!IsNewerSequence(message)) {
    return GooseProcessResult::REJECTED;
  }
  state_ = GooseInputState::ACTIVE;
  stateNumber_ = message.stateNumber;
  sequenceNumber_ = message.sequenceNumber;
  if (nowMs > std::numeric_limits<std::int64_t>::max() -
                  message.timeAllowedToLiveMs) {
    expiresAtMs_ = std::numeric_limits<std::int64_t>::max();
  } else {
    expiresAtMs_ = nowMs + message.timeAllowedToLiveMs;
  }
  values_ = message.values;
  return GooseProcessResult::ACCEPTED;
}

GooseProcessResult GooseStateMachine::CheckTimeout(std::int64_t nowMs) {
  if (state_ != GooseInputState::ACTIVE || nowMs < expiresAtMs_) {
    return GooseProcessResult::NO_CHANGE;
  }
  state_ = GooseInputState::TIMED_OUT;
  return GooseProcessResult::TIMED_OUT;
}

void GooseStateMachine::Reset() {
  state_ = GooseInputState::NOT_RECEIVED;
  stateNumber_ = 0;
  sequenceNumber_ = 0;
  expiresAtMs_ = 0;
  values_.clear();
}

GooseInputState GooseStateMachine::state() const { return state_; }

std::uint32_t GooseStateMachine::stateNumber() const { return stateNumber_; }

std::uint32_t GooseStateMachine::sequenceNumber() const {
  return sequenceNumber_;
}

const std::vector<MmsDataValue>& GooseStateMachine::values() const {
  return values_;
}

}  // namespace IEC61850
