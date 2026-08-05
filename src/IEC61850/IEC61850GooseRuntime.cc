#include "IEC61850GooseRuntime.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace IEC61850 {
namespace {

class ProcessingGuard {
public:
  explicit ProcessingGuard(GooseRealtimeEngine* engine) noexcept
      : engine_(engine), locked_(engine != nullptr && engine->TryLock()) {}
  ~ProcessingGuard() {
    if (locked_) {
      engine_->Unlock();
    }
  }
  bool locked() const noexcept { return locked_; }

private:
  GooseRealtimeEngine* engine_;
  bool locked_;
};

}  // namespace

GooseRealtimeEngine::GooseRealtimeEngine(
    std::vector<GooseRealtimeSubscriptionConfig> subscriptions) {
  subscriptions_.reserve(subscriptions.size());
  for (auto& config : subscriptions) {
    if (config.signalIds.size() != config.valueTypes.size()) {
      continue;
    }
    SubscriptionState state;
    state.values.resize(config.valueTypes.size());
    state.config = std::move(config);
    subscriptions_.emplace_back(std::move(state));
  }
}

bool GooseRealtimeEngine::TryLock() noexcept {
  return !processing_.test_and_set(std::memory_order_acquire);
}

void GooseRealtimeEngine::Unlock() noexcept {
  processing_.clear(std::memory_order_release);
}

std::int64_t GooseRealtimeEngine::SaturatingExpiry(
    std::int64_t nowNs, std::uint32_t ttlMs) noexcept {
  constexpr std::int64_t kNsPerMs = 1'000'000;
  if (ttlMs > static_cast<std::uint64_t>(INT64_MAX / kNsPerMs)) {
    return INT64_MAX;
  }
  const auto duration = static_cast<std::int64_t>(ttlMs) * kNsPerMs;
  if (nowNs > INT64_MAX - duration) {
    return INT64_MAX;
  }
  return nowNs + duration;
}

bool GooseRealtimeEngine::EqualValue(const ProtocolRealtimeValue& left,
                                     const ProtocolRealtimeValue& right) noexcept {
  if (left.valueType != right.valueType || left.qualityBits != right.qualityBits) {
    return false;
  }
  switch (left.valueType) {
    case ProtocolRealtimeValueType::BOOLEAN:
      return left.value.booleanValue == right.value.booleanValue;
    case ProtocolRealtimeValueType::INTEGER:
      return left.value.integerValue == right.value.integerValue;
    case ProtocolRealtimeValueType::FLOATING:
      return left.value.floatingValue == right.value.floatingValue;
  }
  return false;
}

GooseRealtimeProcessResult GooseRealtimeEngine::Reject() noexcept {
  ++statistics_.rejected;
  return GooseRealtimeProcessResult::REJECTED;
}

GooseRealtimeProcessResult GooseRealtimeEngine::TryProcess(
    const ProtocolGooseFrameView& frame, std::int64_t nowNs,
    std::size_t* acceptedRoute) noexcept {
  if (acceptedRoute != nullptr) {
    *acceptedRoute = 0;
  }
  ProcessingGuard guard(this);
  if (!guard.locked()) {
    ++statistics_.busy;
    return GooseRealtimeProcessResult::REJECTED;
  }
  const auto route = std::find_if(
      subscriptions_.begin(), subscriptions_.end(), [&frame](const auto& item) {
        return item.config.subscriptionId == frame.subscriptionId;
      });
  if (route == subscriptions_.end()) {
    return Reject();
  }
  auto& current = *route;
  const auto channelIndex = static_cast<std::size_t>(frame.channel);
  if (channelIndex >= current.config.appIds.size() ||
      current.config.appIds[channelIndex] == 0 ||
      current.config.appIds[channelIndex] != frame.appId ||
      frame.gocbRef != current.config.gocbRef ||
      frame.dataSetRef != current.config.dataSetRef ||
      frame.goId != current.config.goId ||
      frame.configRevision != current.config.configRevision ||
      frame.timeAllowedToLiveMs == 0 || frame.simulation ||
      frame.needsCommissioning ||
      frame.values.size() != current.config.valueTypes.size()) {
    return Reject();
  }
  for (std::size_t index = 0; index < frame.values.size(); ++index) {
    if (frame.values[index].valueType != current.config.valueTypes[index]) {
      return Reject();
    }
  }
  if (current.hasSequence && frame.stateNumber == current.stateNumber &&
      frame.sequenceNumber == current.sequenceNumber) {
    if (!std::equal(frame.values.begin(), frame.values.end(),
                    current.values.begin(), EqualValue)) {
      ++statistics_.conflict;
      return GooseRealtimeProcessResult::CONFLICT;
    }
    const bool recovered = current.state == GooseRealtimeInputState::TIMED_OUT;
    current.state = GooseRealtimeInputState::ACTIVE;
    current.expiresAtNs = SaturatingExpiry(nowNs, frame.timeAllowedToLiveMs);
    ++statistics_.duplicate;
    return recovered ? GooseRealtimeProcessResult::RECOVERED
                     : GooseRealtimeProcessResult::DUPLICATE;
  }
  if (current.hasSequence) {
    if (frame.stateNumber < current.stateNumber ||
        (frame.stateNumber == current.stateNumber
             ? frame.sequenceNumber <= current.sequenceNumber
             : frame.sequenceNumber != 0)) {
      return Reject();
    }
  } else if (frame.stateNumber == 0) {
    return Reject();
  }
  std::copy(frame.values.begin(), frame.values.end(), current.values.begin());
  current.hasSequence = true;
  current.state = GooseRealtimeInputState::ACTIVE;
  current.stateNumber = frame.stateNumber;
  current.sequenceNumber = frame.sequenceNumber;
  current.expiresAtNs = SaturatingExpiry(nowNs, frame.timeAllowedToLiveMs);
  ++statistics_.accepted;
  if (acceptedRoute != nullptr) {
    *acceptedRoute = static_cast<std::size_t>(route - subscriptions_.begin());
  }
  return GooseRealtimeProcessResult::ACCEPTED;
}

GooseRealtimeProcessResult GooseRealtimeEngine::CheckTimeout(
    std::int64_t nowNs, std::size_t* timedOutRoute,
    std::span<ProtocolRealtimeValue> timedOutValues,
    std::size_t* timedOutValueCount) noexcept {
  if (timedOutRoute != nullptr) {
    *timedOutRoute = 0;
  }
  if (timedOutValueCount != nullptr) {
    *timedOutValueCount = 0;
  }
  ProcessingGuard guard(this);
  if (!guard.locked()) {
    ++statistics_.busy;
    return GooseRealtimeProcessResult::NO_CHANGE;
  }
  for (std::size_t index = 0; index < subscriptions_.size(); ++index) {
    auto& current = subscriptions_[index];
    if (current.state != GooseRealtimeInputState::ACTIVE ||
        nowNs < current.expiresAtNs) {
      continue;
    }
    current.state = GooseRealtimeInputState::TIMED_OUT;
    ++statistics_.timedOut;
    if (timedOutRoute != nullptr) {
      *timedOutRoute = index;
    }
    if (timedOutValueCount != nullptr) {
      *timedOutValueCount = current.values.size();
    }
    if (timedOutValues.size() >= current.values.size()) {
      std::copy(current.values.begin(), current.values.end(),
                timedOutValues.begin());
    }
    return GooseRealtimeProcessResult::TIMED_OUT;
  }
  return GooseRealtimeProcessResult::NO_CHANGE;
}

std::span<const ProtocolRealtimeValue> GooseRealtimeEngine::values(
    std::size_t route) const noexcept {
  if (route >= subscriptions_.size()) {
    return {};
  }
  return subscriptions_[route].values;
}

std::span<const std::uint32_t> GooseRealtimeEngine::signalIds(
    std::size_t route) const noexcept {
  if (route >= subscriptions_.size()) {
    return {};
  }
  return subscriptions_[route].config.signalIds;
}

GooseRealtimeInputState GooseRealtimeEngine::state(std::size_t route) const noexcept {
  return route < subscriptions_.size()
             ? subscriptions_[route].state
             : GooseRealtimeInputState::NOT_RECEIVED;
}

const GooseRealtimeStatistics& GooseRealtimeEngine::statistics() const noexcept {
  return statistics_;
}

std::size_t GooseRealtimeEngine::size() const noexcept {
  return subscriptions_.size();
}

}  // namespace IEC61850
