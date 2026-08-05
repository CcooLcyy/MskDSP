#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace IEC61850 {

enum class SvNetworkChannel : std::uint8_t {
  UNSPECIFIED = 0,
  A = 1,
  B = 2,
};

struct SvChannelSample {
  std::int32_t value = 0;
  std::uint32_t quality = 0;
  bool operator==(const SvChannelSample&) const = default;
};

static_assert(std::is_trivially_copyable_v<SvChannelSample>);

struct SvSampleMetadata {
  std::uint16_t appId = 0;
  SvNetworkChannel channel = SvNetworkChannel::UNSPECIFIED;
  std::uint16_t smpCnt = 0;
  std::uint8_t smpSynch = 0;
  std::int64_t receiveTimestampNs = 0;
};

struct SvAsduView {
  std::string_view svId;
  std::uint16_t smpCnt = 0;
  std::uint32_t configRevision = 0;
  std::uint8_t smpSynch = 0;
  std::span<const SvChannelSample> samples;
};

struct SvFrameView {
  std::uint16_t appId = 0;
  SvNetworkChannel channel = SvNetworkChannel::UNSPECIFIED;
  std::int64_t receiveTimestampNs = 0;
  std::span<const SvAsduView> asdus;
};

struct SvReceiveConfig {
  std::uint16_t appId = 0;
  std::string svId;
  std::uint32_t configRevision = 0;
  std::size_t expectedAsduCount = 0;
  std::size_t channelCount = 0;
  std::uint8_t expectedSmpSynch = 0;
  std::size_t bufferCapacity = 0;
};

enum class SvBufferPushResult {
  ENQUEUED,
  OVERWROTE_OLDEST,
  REJECTED_LAYOUT,
};

class SvSampleRingBuffer {
public:
  SvSampleRingBuffer(std::size_t capacity, std::size_t channelCount);

  bool valid() const noexcept;
  SvBufferPushResult Push(const SvSampleMetadata& metadata,
                          std::span<const SvChannelSample> samples) noexcept;
  bool TryPop(SvSampleMetadata* metadata,
              std::span<SvChannelSample> samples) noexcept;
  void Clear() noexcept;
  bool empty() const noexcept;
  std::size_t size() const noexcept;
  std::size_t capacity() const noexcept;
  std::size_t channelCount() const noexcept;

private:
  struct Slot {
    SvSampleMetadata metadata;
    std::vector<SvChannelSample> samples;
  };

  std::vector<Slot> slots_;
  std::size_t channelCount_ = 0;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

enum class SvRejectReason {
  NONE,
  INVALID_CONFIGURATION,
  APP_ID_MISMATCH,
  SV_ID_MISMATCH,
  ASDU_COUNT_MISMATCH,
  CONFIG_REVISION_MISMATCH,
  SMP_SYNCH_INVALID,
  SMP_SYNCH_MISMATCH,
  SAMPLE_LAYOUT_MISMATCH,
  DUPLICATE_SAMPLE,
  OUT_OF_ORDER_SAMPLE,
};

struct SvProcessResult {
  bool accepted = false;
  bool sequenceGap = false;
  bool bufferOverflow = false;
  std::uint32_t missingSamples = 0;
  std::uint32_t overwrittenSamples = 0;
  SvRejectReason rejectReason = SvRejectReason::NONE;
};

struct SvReceiveStatistics {
  std::uint64_t framesReceived = 0;
  std::uint64_t framesAccepted = 0;
  std::uint64_t framesInvalid = 0;
  std::uint64_t asdusAccepted = 0;
  std::uint64_t duplicateFrames = 0;
  std::uint64_t outOfOrderFrames = 0;
  std::uint64_t sequenceGapEvents = 0;
  std::uint64_t samplesDropped = 0;
  std::uint64_t samplesOverwritten = 0;
  std::uint64_t bufferOverflowEvents = 0;
  std::uint64_t bufferHighWatermark = 0;
  std::uint64_t sessionResets = 0;
};

class SvReceiveState {
public:
  explicit SvReceiveState(SvReceiveConfig config);

  SvProcessResult Process(const SvFrameView& frame);
  bool TryPop(SvSampleMetadata* metadata,
              std::span<SvChannelSample> samples) noexcept;
  void ResetSession() noexcept;

  bool configValid() const noexcept;
  bool hasLastSmpCnt() const noexcept;
  std::uint16_t lastSmpCnt() const noexcept;
  std::size_t bufferedSamples() const noexcept;
  std::size_t bufferCapacity() const noexcept;
  const SvReceiveStatistics& statistics() const noexcept;

private:
  SvProcessResult Reject(SvRejectReason reason) noexcept;
  bool ValidSmpSynch(std::uint8_t value) const noexcept;

  SvReceiveConfig config_;
  bool configValid_ = false;
  bool hasLastSmpCnt_ = false;
  std::uint16_t lastSmpCnt_ = 0;
  SvSampleRingBuffer buffer_{0, 0};
  SvReceiveStatistics statistics_;
};

}  // namespace IEC61850
