#include "IEC61850SvState.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace IEC61850 {
namespace {

constexpr std::uint8_t kMaxSmpSynch = 2;
constexpr std::uint16_t kHalfSequence = 0x8000;

std::uint64_t SaturatingAdd(std::uint64_t value,
                            std::uint64_t increment) noexcept {
  if (increment > std::numeric_limits<std::uint64_t>::max() - value) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return value + increment;
}

}  // namespace

SvSampleRingBuffer::SvSampleRingBuffer(std::size_t capacity,
                                       std::size_t channelCount)
    : channelCount_(channelCount) {
  if (capacity == 0 || channelCount == 0 ||
      capacity > std::numeric_limits<std::size_t>::max() / channelCount) {
    channelCount_ = 0;
    return;
  }
  try {
    slots_.resize(capacity);
  } catch (...) {
    channelCount_ = 0;
    return;
  }
  for (auto& slot : slots_) {
    try {
      slot.samples.resize(channelCount);
    } catch (...) {
      slots_.clear();
      channelCount_ = 0;
      return;
    }
  }
}

bool SvSampleRingBuffer::valid() const noexcept {
  return !slots_.empty() && channelCount_ != 0;
}

SvBufferPushResult SvSampleRingBuffer::Push(
    const SvSampleMetadata& metadata,
    std::span<const SvChannelSample> samples) noexcept {
  if (!valid() || samples.size() != channelCount_) {
    return SvBufferPushResult::REJECTED_LAYOUT;
  }
  const bool overwrite = size_ == slots_.size();
  auto& slot = slots_[head_];
  slot.metadata = metadata;
  std::copy(samples.begin(), samples.end(), slot.samples.begin());
  head_ = (head_ + 1) % slots_.size();
  if (!overwrite) {
    ++size_;
  }
  return overwrite ? SvBufferPushResult::OVERWROTE_OLDEST
                   : SvBufferPushResult::ENQUEUED;
}

bool SvSampleRingBuffer::TryPop(SvSampleMetadata* metadata,
                                std::span<SvChannelSample> samples) noexcept {
  if (!valid() || size_ == 0 || metadata == nullptr ||
      samples.size() != channelCount_) {
    return false;
  }
  const auto tail = (head_ + slots_.size() - size_) % slots_.size();
  const auto& slot = slots_[tail];
  *metadata = slot.metadata;
  std::copy(slot.samples.begin(), slot.samples.end(), samples.begin());
  --size_;
  return true;
}

void SvSampleRingBuffer::Clear() noexcept {
  head_ = 0;
  size_ = 0;
}

bool SvSampleRingBuffer::empty() const noexcept { return size_ == 0; }
std::size_t SvSampleRingBuffer::size() const noexcept { return size_; }
std::size_t SvSampleRingBuffer::capacity() const noexcept { return slots_.size(); }
std::size_t SvSampleRingBuffer::channelCount() const noexcept { return channelCount_; }

SvReceiveState::SvReceiveState(SvReceiveConfig config)
    : config_(std::move(config)),
      configValid_(config_.appId != 0 && !config_.svId.empty() &&
                   config_.configRevision != 0 &&
                   config_.expectedAsduCount != 0 &&
                   config_.channelCount != 0 &&
                   config_.bufferCapacity != 0 &&
                   config_.bufferCapacity >= config_.expectedAsduCount &&
                   config_.bufferCapacity <=
                       std::numeric_limits<std::size_t>::max() /
                           config_.channelCount &&
                   config_.expectedSmpSynch <= kMaxSmpSynch),
      buffer_(configValid_ ? config_.bufferCapacity : 0,
              configValid_ ? config_.channelCount : 0) {}

SvProcessResult SvReceiveState::Reject(SvRejectReason reason) noexcept {
  ++statistics_.framesInvalid;
  if (reason == SvRejectReason::DUPLICATE_SAMPLE) {
    ++statistics_.duplicateFrames;
  } else if (reason == SvRejectReason::OUT_OF_ORDER_SAMPLE) {
    ++statistics_.outOfOrderFrames;
  }
  SvProcessResult result;
  result.rejectReason = reason;
  return result;
}

bool SvReceiveState::ValidSmpSynch(std::uint8_t value) const noexcept {
  return value <= kMaxSmpSynch;
}

SvProcessResult SvReceiveState::Process(const SvFrameView& frame) {
  ++statistics_.framesReceived;
  if (!configValid_) {
    return Reject(SvRejectReason::INVALID_CONFIGURATION);
  }
  if (frame.appId != config_.appId) {
    return Reject(SvRejectReason::APP_ID_MISMATCH);
  }
  if (frame.asdus.size() != config_.expectedAsduCount) {
    return Reject(SvRejectReason::ASDU_COUNT_MISMATCH);
  }

  for (const auto& asdu : frame.asdus) {
    if (asdu.svId != config_.svId) {
      return Reject(SvRejectReason::SV_ID_MISMATCH);
    }
    if (asdu.configRevision != config_.configRevision) {
      return Reject(SvRejectReason::CONFIG_REVISION_MISMATCH);
    }
    if (!ValidSmpSynch(asdu.smpSynch)) {
      return Reject(SvRejectReason::SMP_SYNCH_INVALID);
    }
    if (asdu.smpSynch != config_.expectedSmpSynch) {
      return Reject(SvRejectReason::SMP_SYNCH_MISMATCH);
    }
    if (asdu.samples.size() != config_.channelCount) {
      return Reject(SvRejectReason::SAMPLE_LAYOUT_MISMATCH);
    }
  }

  bool localHasLast = hasLastSmpCnt_;
  std::uint16_t localLast = lastSmpCnt_;
  SvProcessResult result;
  for (const auto& asdu : frame.asdus) {
    if (localHasLast) {
      const auto delta = static_cast<std::uint16_t>(asdu.smpCnt - localLast);
      if (delta == 0) {
        return Reject(SvRejectReason::DUPLICATE_SAMPLE);
      }
      if (delta > kHalfSequence) {
        return Reject(SvRejectReason::OUT_OF_ORDER_SAMPLE);
      }
      if (delta > 1) {
        result.sequenceGap = true;
        result.missingSamples += static_cast<std::uint32_t>(delta - 1);
      }
    }
    localLast = asdu.smpCnt;
    localHasLast = true;
  }

  result.accepted = true;
  if (result.sequenceGap) {
    ++statistics_.sequenceGapEvents;
    statistics_.samplesDropped = SaturatingAdd(
        statistics_.samplesDropped, result.missingSamples);
  }
  for (const auto& asdu : frame.asdus) {
    SvSampleMetadata metadata;
    metadata.appId = frame.appId;
    metadata.channel = frame.channel;
    metadata.smpCnt = asdu.smpCnt;
    metadata.smpSynch = asdu.smpSynch;
    metadata.receiveTimestampNs = frame.receiveTimestampNs;
    const auto pushResult = buffer_.Push(metadata, asdu.samples);
    if (pushResult == SvBufferPushResult::REJECTED_LAYOUT) {
      return Reject(SvRejectReason::SAMPLE_LAYOUT_MISMATCH);
    }
    if (pushResult == SvBufferPushResult::OVERWROTE_OLDEST) {
      result.bufferOverflow = true;
      ++result.overwrittenSamples;
      ++statistics_.bufferOverflowEvents;
      statistics_.samplesOverwritten =
          SaturatingAdd(statistics_.samplesOverwritten, 1);
      statistics_.samplesDropped =
          SaturatingAdd(statistics_.samplesDropped, 1);
    }
  }
  hasLastSmpCnt_ = localHasLast;
  lastSmpCnt_ = localLast;
  statistics_.framesAccepted =
      SaturatingAdd(statistics_.framesAccepted, 1);
  statistics_.asdusAccepted = SaturatingAdd(
      statistics_.asdusAccepted, static_cast<std::uint64_t>(frame.asdus.size()));
  statistics_.bufferHighWatermark =
      std::max<std::uint64_t>(statistics_.bufferHighWatermark, buffer_.size());
  return result;
}

bool SvReceiveState::TryPop(SvSampleMetadata* metadata,
                            std::span<SvChannelSample> samples) noexcept {
  return buffer_.TryPop(metadata, samples);
}

void SvReceiveState::ResetSession() noexcept {
  buffer_.Clear();
  hasLastSmpCnt_ = false;
  lastSmpCnt_ = 0;
  ++statistics_.sessionResets;
}

bool SvReceiveState::configValid() const noexcept { return configValid_; }
bool SvReceiveState::hasLastSmpCnt() const noexcept { return hasLastSmpCnt_; }
std::uint16_t SvReceiveState::lastSmpCnt() const noexcept { return lastSmpCnt_; }
std::size_t SvReceiveState::bufferedSamples() const noexcept { return buffer_.size(); }
std::size_t SvReceiveState::bufferCapacity() const noexcept { return buffer_.capacity(); }
const SvReceiveStatistics& SvReceiveState::statistics() const noexcept { return statistics_; }

}  // namespace IEC61850
