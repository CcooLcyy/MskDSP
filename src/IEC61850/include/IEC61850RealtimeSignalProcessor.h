#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "IEC61850ProtocolStack.h"
#include "IEC61850RealtimeSignalBus.h"

namespace IEC61850 {

struct RealtimeSignalSnapshot {
  std::uint32_t signalId = 0;
  std::uint64_t sessionGeneration = 0;
  RealtimeSignalSource source = RealtimeSignalSource::INTERNAL;
  RealtimeSignalValueType valueType = RealtimeSignalValueType::BOOLEAN;
  RealtimeNetworkChannel channel = RealtimeNetworkChannel::UNSPECIFIED;
  std::uint32_t qualityBits = 0;
  std::int64_t timestampNs = 0;
  std::uint64_t sequence = 0;
  RealtimeSignalScalar value{};
  bool valid = false;
};

enum class RealtimeSignalProcessResult : std::uint8_t {
  ACCEPTED = 0,
  UNKNOWN_SIGNAL = 1,
  SESSION_MISMATCH = 2,
  TYPE_MISMATCH = 3,
};

struct RealtimeSignalProcessorStatistics {
  std::uint64_t accepted = 0;
  std::uint64_t unknownSignal = 0;
  std::uint64_t sessionMismatch = 0;
  std::uint64_t typeMismatch = 0;
};

// 将实时总线更新固化为下位机内部快照。处理路径不分配内存、不调用外部服务。
class RealtimeSignalProcessor {
public:
  RealtimeSignalProcessor(
      std::span<const ProtocolSignalDefinition> definitions,
      std::uint64_t sessionGeneration);
  ~RealtimeSignalProcessor();

  RealtimeSignalProcessor(const RealtimeSignalProcessor&) = delete;
  RealtimeSignalProcessor& operator=(const RealtimeSignalProcessor&) = delete;

  RealtimeSignalProcessResult Process(
      const RealtimeSignalUpdate& update) noexcept;

  // 读取一个已经发布的快照；采用版本校验避免读到半份标量。
  bool Load(std::uint32_t signalId,
            RealtimeSignalSnapshot* snapshot) const noexcept;

  std::size_t size() const noexcept;
  std::uint64_t sessionGeneration() const noexcept;
  RealtimeSignalProcessorStatistics statistics() const noexcept;

private:
  struct Slot;

  static RealtimeSignalValueType ToRealtimeType(
      IEC61850Proto::PointValueType valueType) noexcept;
  static std::uint64_t EncodeValue(const RealtimeSignalUpdate& update) noexcept;
  static void DecodeValue(std::uint64_t encoded,
                          RealtimeSignalValueType valueType,
                          RealtimeSignalScalar* value) noexcept;
  std::size_t FindSlot(std::uint32_t signalId) const noexcept;

  std::uint64_t sessionGeneration_ = 0;
  std::vector<std::uint32_t> signalIds_;
  std::unique_ptr<Slot[]> slots_;
  std::atomic<std::uint64_t> accepted_ = 0;
  std::atomic<std::uint64_t> unknownSignal_ = 0;
  std::atomic<std::uint64_t> sessionMismatch_ = 0;
  std::atomic<std::uint64_t> typeMismatch_ = 0;
};

}  // namespace IEC61850
