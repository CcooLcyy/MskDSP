#include "IEC61850SvMathEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include "IEC61850SvMath.h"

namespace IEC61850 {
namespace {

constexpr std::size_t kMaxSvMathWindow = 4096;

bool IsFinite(double value) noexcept { return std::isfinite(value); }

}  // namespace

SvMathEngine::SvMathEngine(std::vector<SvMathStreamPlan> plans,
                           std::uint64_t sessionGeneration)
    : sessionGeneration_(sessionGeneration) {
  if (sessionGeneration_ == 0) {
    return;
  }
  streams_.reserve(plans.size());
  for (auto& plan : plans) {
    if (plan.streamId == 0 || plan.expectedAsduCount != 1 ||
        plan.samplesPerCycle == 0 ||
        plan.samplesPerCycle > kMaxSvMathWindow ||
        !IsFinite(plan.nominalFrequencyHz) ||
        plan.nominalFrequencyHz <= 0.0 || plan.members.empty()) {
      continue;
    }
    StreamState state;
    state.plan = std::move(plan);
    state.pendingValues.resize(state.plan.members.size());
    state.pendingSeen.resize(state.plan.members.size(), false);
    if (state.plan.members.size() >
        std::numeric_limits<std::size_t>::max() /
            state.plan.samplesPerCycle) {
      continue;
    }
    state.history.resize(state.plan.members.size() *
                         state.plan.samplesPerCycle);
    streams_.emplace_back(std::move(state));
  }
}

std::size_t SvMathEngine::Process(
    const RealtimeSignalUpdate& update, std::int64_t nowNs,
    std::span<RealtimeSignalUpdate> outputs) noexcept {
  if (sessionGeneration_ == 0 || update.sessionGeneration != sessionGeneration_ ||
      update.source != RealtimeSignalSource::SV_DERIVED) {
    return 0;
  }

  for (auto& stream : streams_) {
    auto memberIt = std::find_if(
        stream.plan.members.begin(), stream.plan.members.end(),
        [&update](const auto& member) {
          return member.inputSignalId == update.signalId;
        });
    if (memberIt == stream.plan.members.end()) {
      continue;
    }
    const auto memberIndex = static_cast<std::size_t>(
        memberIt - stream.plan.members.begin());
    const auto reset = [&stream] {
      stream.pendingValid = false;
      std::fill(stream.pendingSeen.begin(), stream.pendingSeen.end(), false);
      stream.hasCommittedSample = false;
      stream.historyWriteIndex = 0;
      stream.historyCount = 0;
    };
    if (update.qualityBits != 0 ||
        (update.valueType != RealtimeSignalValueType::INTEGER &&
         update.valueType != RealtimeSignalValueType::FLOATING)) {
      reset();
      return 0;
    }

    const auto sampleCount = static_cast<std::uint16_t>(update.sequence >> 32);
    const auto asduIndex = static_cast<std::uint32_t>(update.sequence);
    if (asduIndex != 0) {
      reset();
      return 0;
    }
    if (!stream.pendingValid) {
      stream.pendingValid = true;
      stream.pendingSampleCount = sampleCount;
      std::fill(stream.pendingSeen.begin(), stream.pendingSeen.end(), false);
    } else if (stream.pendingSampleCount != sampleCount) {
      reset();
      stream.pendingValid = true;
      stream.pendingSampleCount = sampleCount;
      std::fill(stream.pendingSeen.begin(), stream.pendingSeen.end(), false);
    }
    const auto value =
        update.valueType == RealtimeSignalValueType::INTEGER
            ? static_cast<double>(update.value.integerValue)
            : update.value.floatingValue;
    if (!IsFinite(value)) {
      reset();
      return 0;
    }
    stream.pendingValues[memberIndex] = value;
    stream.pendingSeen[memberIndex] = true;
    if (!std::all_of(stream.pendingSeen.begin(), stream.pendingSeen.end(),
                     [](bool seen) { return seen; })) {
      return 0;
    }

    bool sequenceGap = false;
    if (stream.hasCommittedSample) {
      const auto delta = static_cast<std::uint16_t>(
          stream.pendingSampleCount - stream.lastSampleCount);
      sequenceGap = delta != 1;
    }
    if (sequenceGap) {
      stream.historyWriteIndex = 0;
      stream.historyCount = 0;
    }
    for (std::size_t index = 0; index < stream.pendingValues.size(); ++index) {
      stream.history[index * stream.plan.samplesPerCycle +
                     stream.historyWriteIndex] = stream.pendingValues[index];
    }
    stream.historyWriteIndex =
        (stream.historyWriteIndex + 1) % stream.plan.samplesPerCycle;
    stream.historyCount = std::min(stream.historyCount + 1,
                                   stream.plan.samplesPerCycle);
    stream.lastSampleCount = stream.pendingSampleCount;
    stream.hasCommittedSample = true;
    stream.pendingValid = false;
    std::fill(stream.pendingSeen.begin(), stream.pendingSeen.end(), false);

    if (sequenceGap || stream.historyCount < stream.plan.samplesPerCycle) {
      return 0;
    }

    std::size_t outputCount = 0;
    for (const auto& member : stream.plan.members) {
      if (member.rmsSignalId != 0) {
        ++outputCount;
      }
    }
    if (outputCount == 0 || outputs.size() < outputCount) {
      return 0;
    }

    std::array<double, kMaxSvMathWindow> scratch{};
    std::size_t outputIndex = 0;
    const auto oldest = stream.historyWriteIndex;
    for (std::size_t index = 0; index < stream.plan.members.size(); ++index) {
      const auto& member = stream.plan.members[index];
      if (member.rmsSignalId == 0) {
        continue;
      }
      const auto base = index * stream.plan.samplesPerCycle;
      for (std::size_t sample = 0; sample < stream.plan.samplesPerCycle;
           ++sample) {
        scratch[sample] =
            stream.history[base + (oldest + sample) % stream.plan.samplesPerCycle];
      }
      const auto result = ComputeRms(
          std::span<const double>(scratch.data(), stream.plan.samplesPerCycle));
      if (!result.ok()) {
        return 0;
      }
      auto& output = outputs[outputIndex++];
      output = {};
      output.signalId = member.rmsSignalId;
      output.sessionGeneration = sessionGeneration_;
      output.source = RealtimeSignalSource::SV_DERIVED;
      output.valueType = RealtimeSignalValueType::FLOATING;
      output.channel = RealtimeNetworkChannel::UNSPECIFIED;
      output.timestampNs = nowNs;
      output.sequence = (static_cast<std::uint64_t>(stream.lastSampleCount)
                         << 32);
      output.value.floatingValue = *result.value;
    }
    return outputIndex;
  }
  return 0;
}

std::size_t SvMathEngine::streamCount() const noexcept {
  return streams_.size();
}

std::size_t SvMathEngine::maxOutputCount() const noexcept {
  std::size_t result = 0;
  for (const auto& stream : streams_) {
    std::size_t count = 0;
    for (const auto& member : stream.plan.members) {
      if (member.rmsSignalId != 0) {
        ++count;
      }
    }
    result = std::max(result, count);
  }
  return result;
}

std::uint64_t SvMathEngine::sessionGeneration() const noexcept {
  return sessionGeneration_;
}

}  // namespace IEC61850
