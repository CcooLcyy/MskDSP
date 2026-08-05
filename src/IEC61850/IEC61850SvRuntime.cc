#include "IEC61850SvRuntime.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace IEC61850 {
namespace {

constexpr std::size_t kMaxSvSampleWindow = 4096;

class ProcessingGuard {
public:
  explicit ProcessingGuard(SvRealtimeEngine* engine) noexcept
      : engine_(engine), locked_(engine != nullptr && engine->TryLock()) {}
  ~ProcessingGuard() {
    if (locked_) {
      engine_->Unlock();
    }
  }
  bool locked() const noexcept { return locked_; }

private:
  SvRealtimeEngine* engine_;
  bool locked_;
};

}  // namespace

SvRealtimeEngine::SvRealtimeEngine(
    std::vector<SvRealtimeSubscriptionConfig> subscriptions) {
  subscriptions_.reserve(subscriptions.size());
  for (auto& config : subscriptions) {
    if (config.streamId == 0 || config.svId.empty() ||
        config.configRevision == 0 || config.expectedAsduCount == 0 ||
        config.valueTypes.empty()) {
      continue;
    }
    bool duplicateAppId = false;
    for (std::size_t left = 0; left < config.appIds.size(); ++left) {
      for (std::size_t right = left + 1; right < config.appIds.size(); ++right) {
        if (config.appIds[left] != 0 &&
            config.appIds[left] == config.appIds[right]) {
          duplicateAppId = true;
        }
      }
    }
    if (duplicateAppId) {
      continue;
    }
    SubscriptionState state;
    state.values.resize(config.valueTypes.size());
    state.recentAsdus.resize(config.expectedAsduCount);
    for (auto& recent : state.recentAsdus) {
      recent.values.resize(config.valueTypes.size());
    }
    if (config.sampleWindowSize != 0 &&
        config.sampleWindowSize <= kMaxSvSampleWindow &&
        config.sampleWindowSize <=
            std::numeric_limits<std::size_t>::max() /
                config.valueTypes.size()) {
      state.sampleWindowSize = config.sampleWindowSize;
      state.sampleWindow.resize(config.valueTypes.size() *
                                state.sampleWindowSize);
    }
    state.config = std::move(config);
    subscriptions_.emplace_back(std::move(state));
  }
}

bool SvRealtimeEngine::TryLock() const noexcept {
  return !processing_.test_and_set(std::memory_order_acquire);
}

void SvRealtimeEngine::Unlock() const noexcept {
  processing_.clear(std::memory_order_release);
}

bool SvRealtimeEngine::EqualValue(const ProtocolRealtimeValue& left,
                                  const ProtocolRealtimeValue& right) noexcept {
  if (left.valueType != right.valueType ||
      left.qualityBits != right.qualityBits) {
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

SvRealtimeProcessResult SvRealtimeEngine::Reject() noexcept {
  ++statistics_.rejected;
  return SvRealtimeProcessResult::REJECTED;
}

SvRealtimeProcessResult SvRealtimeEngine::TryProcess(
    const ProtocolSvFrameView& frame, std::size_t* acceptedRoute,
    std::uint32_t* missingSamples) noexcept {
  if (acceptedRoute != nullptr) {
    *acceptedRoute = 0;
  }
  if (missingSamples != nullptr) {
    *missingSamples = 0;
  }
  ProcessingGuard guard(this);
  if (!guard.locked()) {
    ++statistics_.busy;
    return SvRealtimeProcessResult::REJECTED;
  }
  const auto route = std::find_if(
      subscriptions_.begin(), subscriptions_.end(), [&frame](const auto& item) {
        return item.config.streamId == frame.streamId;
      });
  if (route == subscriptions_.end()) {
    return Reject();
  }
  auto& current = *route;
  const auto channelIndex = static_cast<std::size_t>(frame.channel);
  if (channelIndex >= current.config.appIds.size() ||
      current.config.appIds[channelIndex] == 0 ||
      current.config.appIds[channelIndex] != frame.appId ||
      frame.svId != current.config.svId ||
      frame.configRevision != current.config.configRevision ||
      frame.asduCount != current.config.expectedAsduCount ||
      frame.sampleSynchronization > 2 ||
      (current.config.expectedSampleSynchronization != 0xff &&
       frame.sampleSynchronization !=
           current.config.expectedSampleSynchronization) ||
      frame.asduIndex >= current.config.expectedAsduCount ||
      frame.values.size() != current.config.valueTypes.size()) {
    return Reject();
  }
  for (std::size_t index = 0; index < frame.values.size(); ++index) {
    if (frame.values[index].valueType != current.config.valueTypes[index]) {
      return Reject();
    }
  }
  auto& recent = current.recentAsdus[frame.asduIndex];
  if (recent.valid && recent.sampleCount == frame.sampleCount) {
    if (!std::equal(frame.values.begin(), frame.values.end(),
                    recent.values.begin(), EqualValue)) {
      ++statistics_.conflict;
      return SvRealtimeProcessResult::CONFLICT;
    }
    ++statistics_.duplicate;
    return SvRealtimeProcessResult::DUPLICATE;
  }
  bool sequenceGap = false;
  if (!current.hasSampleCount && frame.asduIndex != 0) {
    return Reject();
  }
  if (current.hasSampleCount) {
    const auto delta = static_cast<std::uint16_t>(
        frame.sampleCount - current.sampleCount);
    const bool nextAsdu =
        current.asduIndex + 1 < current.config.expectedAsduCount &&
        frame.asduIndex == current.asduIndex + 1;
    const bool nextFrame = frame.asduIndex == 0 &&
                           current.asduIndex ==
                               current.config.expectedAsduCount - 1;
    if (!nextAsdu && !nextFrame) {
      return Reject();
    }
    if ((nextAsdu && delta != 0) || (nextFrame && delta == 0) ||
        delta > 0x8000) {
      return Reject();
    }
    if (delta > 1) {
      sequenceGap = true;
      ++statistics_.sequenceGapEvents;
      statistics_.missingSamples += delta - 1;
      if (missingSamples != nullptr) {
        *missingSamples = delta - 1;
      }
    }
  }
  std::copy(frame.values.begin(), frame.values.end(), current.values.begin());
  if (sequenceGap) {
    current.sampleWindowWriteIndex = 0;
    current.sampleWindowValueCount = 0;
  }
  if (current.sampleWindowSize != 0) {
    for (std::size_t index = 0; index < frame.values.size(); ++index) {
      double numericValue = 0.0;
      switch (frame.values[index].valueType) {
        case ProtocolRealtimeValueType::INTEGER:
          numericValue = static_cast<double>(frame.values[index].value.integerValue);
          break;
        case ProtocolRealtimeValueType::FLOATING:
          numericValue = frame.values[index].value.floatingValue;
          break;
        case ProtocolRealtimeValueType::BOOLEAN:
          continue;
      }
      current.sampleWindow[index * current.sampleWindowSize +
                           current.sampleWindowWriteIndex] = numericValue;
    }
    current.sampleWindowWriteIndex =
        (current.sampleWindowWriteIndex + 1) % current.sampleWindowSize;
    current.sampleWindowValueCount = std::min(
        current.sampleWindowValueCount + 1, current.sampleWindowSize);
  }
  recent.valid = true;
  recent.sampleCount = frame.sampleCount;
  std::copy(frame.values.begin(), frame.values.end(), recent.values.begin());
  current.hasSampleCount = true;
  current.sampleCount = frame.sampleCount;
  current.asduIndex = frame.asduIndex;
  ++statistics_.accepted;
  if (acceptedRoute != nullptr) {
    *acceptedRoute = static_cast<std::size_t>(route - subscriptions_.begin());
  }
  if (sequenceGap) {
    return SvRealtimeProcessResult::SEQUENCE_GAP;
  }
  return SvRealtimeProcessResult::ACCEPTED;
}

std::span<const ProtocolRealtimeValue> SvRealtimeEngine::values(
    std::size_t route) const noexcept {
  if (route >= subscriptions_.size()) {
    return {};
  }
  return subscriptions_[route].values;
}

std::size_t SvRealtimeEngine::CopyNumericSamples(
    std::size_t route, std::size_t member,
    std::span<double> output) const noexcept {
  if (output.empty() || !TryLock()) {
    return 0;
  }
  if (route >= subscriptions_.size()) {
    Unlock();
    return 0;
  }
  const auto& current = subscriptions_[route];
  if (member >= current.config.valueTypes.size() ||
      current.config.valueTypes[member] == ProtocolRealtimeValueType::BOOLEAN ||
      current.sampleWindowSize == 0 || current.sampleWindowValueCount == 0) {
    Unlock();
    return 0;
  }
  const auto count = std::min(current.sampleWindowValueCount, output.size());
  const auto oldest =
      (current.sampleWindowWriteIndex + current.sampleWindowSize -
       current.sampleWindowValueCount) %
      current.sampleWindowSize;
  const auto base = member * current.sampleWindowSize;
  const auto skip = current.sampleWindowValueCount - count;
  for (std::size_t index = 0; index < count; ++index) {
    const auto ringIndex =
        (oldest + skip + index) % current.sampleWindowSize;
    output[index] = current.sampleWindow[base + ringIndex];
  }
  Unlock();
  return count;
}

const SvRealtimeStatistics& SvRealtimeEngine::statistics() const noexcept {
  return statistics_;
}

std::size_t SvRealtimeEngine::size() const noexcept {
  return subscriptions_.size();
}

}  // namespace IEC61850
