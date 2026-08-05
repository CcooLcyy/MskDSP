#include "IEC61850RealtimeSignalProcessor.h"

#include <algorithm>
#include <cstring>

namespace IEC61850 {

struct RealtimeSignalProcessor::Slot {
  ProtocolSignalDefinition definition;
  std::atomic<std::uint64_t> version = 0;
  std::atomic<std::uint64_t> encodedValue = 0;
  std::atomic<std::uint32_t> qualityBits = 0;
  std::atomic<std::int64_t> timestampNs = 0;
  std::atomic<std::uint64_t> sequence = 0;
  std::atomic<std::uint8_t> source =
      static_cast<std::uint8_t>(RealtimeSignalSource::INTERNAL);
  std::atomic<std::uint8_t> channel =
      static_cast<std::uint8_t>(RealtimeNetworkChannel::UNSPECIFIED);
  std::atomic<std::uint8_t> valueType =
      static_cast<std::uint8_t>(RealtimeSignalValueType::BOOLEAN);
};

RealtimeSignalProcessor::~RealtimeSignalProcessor() = default;

RealtimeSignalValueType RealtimeSignalProcessor::ToRealtimeType(
    IEC61850Proto::PointValueType valueType) noexcept {
  switch (valueType) {
    case IEC61850Proto::POINT_VALUE_TYPE_BOOL:
      return RealtimeSignalValueType::BOOLEAN;
    case IEC61850Proto::POINT_VALUE_TYPE_INT64:
      return RealtimeSignalValueType::INTEGER;
    case IEC61850Proto::POINT_VALUE_TYPE_DOUBLE:
      return RealtimeSignalValueType::FLOATING;
    default:
      return RealtimeSignalValueType::BOOLEAN;
  }
}

std::uint64_t RealtimeSignalProcessor::EncodeValue(
    const RealtimeSignalUpdate& update) noexcept {
  if (update.valueType == RealtimeSignalValueType::BOOLEAN) {
    return update.value.booleanValue ? 1 : 0;
  }
  std::uint64_t encoded = 0;
  if (update.valueType == RealtimeSignalValueType::INTEGER) {
    std::memcpy(&encoded, &update.value.integerValue, sizeof(encoded));
  } else {
    std::memcpy(&encoded, &update.value.floatingValue, sizeof(encoded));
  }
  return encoded;
}

void RealtimeSignalProcessor::DecodeValue(
    std::uint64_t encoded, RealtimeSignalValueType valueType,
    RealtimeSignalScalar* value) noexcept {
  if (value == nullptr) {
    return;
  }
  if (valueType == RealtimeSignalValueType::BOOLEAN) {
    value->booleanValue = encoded != 0;
  } else if (valueType == RealtimeSignalValueType::INTEGER) {
    std::memcpy(&value->integerValue, &encoded, sizeof(encoded));
  } else {
    std::memcpy(&value->floatingValue, &encoded, sizeof(encoded));
  }
}

RealtimeSignalProcessor::RealtimeSignalProcessor(
    std::span<const ProtocolSignalDefinition> definitions,
    std::uint64_t sessionGeneration)
    : sessionGeneration_(sessionGeneration) {
  std::vector<ProtocolSignalDefinition> ordered(definitions.begin(),
                                                definitions.end());
  std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
    return left.signalId < right.signalId;
  });
  const auto uniqueEnd = std::unique(
      ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.signalId == right.signalId;
      });
  ordered.erase(uniqueEnd, ordered.end());
  for (const auto& definition : ordered) {
    if (definition.signalId == 0 ||
        definition.valueType == IEC61850Proto::POINT_VALUE_TYPE_STRING ||
        definition.valueType == IEC61850Proto::POINT_VALUE_TYPE_BYTES ||
        definition.valueType == IEC61850Proto::POINT_VALUE_TYPE_UNSPECIFIED) {
      continue;
    }
    signalIds_.push_back(definition.signalId);
  }
  slots_ = std::make_unique<Slot[]>(signalIds_.size());
  std::size_t slotIndex = 0;
  for (const auto& definition : ordered) {
    if (slotIndex >= signalIds_.size() || definition.signalId == 0 ||
        definition.valueType == IEC61850Proto::POINT_VALUE_TYPE_STRING ||
        definition.valueType == IEC61850Proto::POINT_VALUE_TYPE_BYTES ||
        definition.valueType == IEC61850Proto::POINT_VALUE_TYPE_UNSPECIFIED) {
      continue;
    }
    slots_[slotIndex++].definition = definition;
  }
}

std::size_t RealtimeSignalProcessor::FindSlot(
    std::uint32_t signalId) const noexcept {
  const auto it = std::lower_bound(signalIds_.begin(), signalIds_.end(),
                                   signalId);
  return it == signalIds_.end() || *it != signalId
             ? signalIds_.size()
             : static_cast<std::size_t>(it - signalIds_.begin());
}

RealtimeSignalProcessResult RealtimeSignalProcessor::Process(
    const RealtimeSignalUpdate& update) noexcept {
  if (update.sessionGeneration != sessionGeneration_) {
    sessionMismatch_.fetch_add(1, std::memory_order_relaxed);
    return RealtimeSignalProcessResult::SESSION_MISMATCH;
  }
  const auto index = FindSlot(update.signalId);
  if (index >= signalIds_.size()) {
    unknownSignal_.fetch_add(1, std::memory_order_relaxed);
    return RealtimeSignalProcessResult::UNKNOWN_SIGNAL;
  }
  const auto expected = ToRealtimeType(slots_[index].definition.valueType);
  if (expected != update.valueType) {
    typeMismatch_.fetch_add(1, std::memory_order_relaxed);
    return RealtimeSignalProcessResult::TYPE_MISMATCH;
  }
  auto& slot = slots_[index];
  const auto version = slot.version.load(std::memory_order_relaxed);
  slot.version.store(version + 1, std::memory_order_relaxed);
  slot.encodedValue.store(EncodeValue(update), std::memory_order_relaxed);
  slot.qualityBits.store(update.qualityBits, std::memory_order_relaxed);
  slot.timestampNs.store(update.timestampNs, std::memory_order_relaxed);
  slot.sequence.store(update.sequence, std::memory_order_relaxed);
  slot.source.store(static_cast<std::uint8_t>(update.source),
                    std::memory_order_relaxed);
  slot.channel.store(static_cast<std::uint8_t>(update.channel),
                     std::memory_order_relaxed);
  slot.valueType.store(static_cast<std::uint8_t>(update.valueType),
                       std::memory_order_relaxed);
  slot.version.store(version + 2, std::memory_order_release);
  accepted_.fetch_add(1, std::memory_order_relaxed);
  return RealtimeSignalProcessResult::ACCEPTED;
}

bool RealtimeSignalProcessor::Load(
    std::uint32_t signalId, RealtimeSignalSnapshot* snapshot) const noexcept {
  if (snapshot == nullptr) {
    return false;
  }
  const auto index = FindSlot(signalId);
  if (index >= signalIds_.size()) {
    return false;
  }
  const auto& slot = slots_[index];
  for (int attempt = 0; attempt != 8; ++attempt) {
    const auto first = slot.version.load(std::memory_order_acquire);
    if ((first & 1U) != 0) {
      continue;
    }
    RealtimeSignalSnapshot result;
    result.signalId = signalId;
    result.sessionGeneration = sessionGeneration_;
    result.qualityBits = slot.qualityBits.load(std::memory_order_relaxed);
    result.timestampNs = slot.timestampNs.load(std::memory_order_relaxed);
    result.sequence = slot.sequence.load(std::memory_order_relaxed);
    result.source = static_cast<RealtimeSignalSource>(
        slot.source.load(std::memory_order_relaxed));
    result.channel = static_cast<RealtimeNetworkChannel>(
        slot.channel.load(std::memory_order_relaxed));
    result.valueType = static_cast<RealtimeSignalValueType>(
        slot.valueType.load(std::memory_order_relaxed));
    DecodeValue(slot.encodedValue.load(std::memory_order_relaxed),
                result.valueType, &result.value);
    const auto second = slot.version.load(std::memory_order_acquire);
    if (first == second) {
      result.valid = first != 0;
      *snapshot = result;
      return result.valid;
    }
  }
  return false;
}

std::size_t RealtimeSignalProcessor::size() const noexcept {
  return signalIds_.size();
}

std::uint64_t RealtimeSignalProcessor::sessionGeneration() const noexcept {
  return sessionGeneration_;
}

RealtimeSignalProcessorStatistics RealtimeSignalProcessor::statistics()
    const noexcept {
  return {.accepted = accepted_.load(std::memory_order_acquire),
          .unknownSignal = unknownSignal_.load(std::memory_order_acquire),
          .sessionMismatch = sessionMismatch_.load(std::memory_order_acquire),
          .typeMismatch = typeMismatch_.load(std::memory_order_acquire)};
}

}  // namespace IEC61850
