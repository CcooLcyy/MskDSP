#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "IEC61850ProtocolStack.h"

namespace IEC61850 {

// 判断IED配置中的SV额定频率是否满足当前下位机支持范围；0表示默认50Hz。
inline bool IsSupportedSvNominalFrequencyHz(double frequencyHz) noexcept {
  return std::isfinite(frequencyHz) &&
         (frequencyHz == 0.0 || frequencyHz == 50.0 || frequencyHz == 60.0);
}

// 将IED配置中的SV额定频率解析为运行时使用值；0表示默认50Hz。
inline double ResolveSvNominalFrequencyHz(double frequencyHz) noexcept {
  return frequencyHz == 0.0 ? 50.0 : frequencyHz;
}

struct SvRealtimeSubscriptionConfig {
  std::uint32_t streamId = 0;
  std::string svId;
  std::uint64_t configRevision = 0;
  std::uint32_t expectedAsduCount = 0;
  // 预分配数值窗口；0表示只保留当前值，不进行派生计算。
  std::size_t sampleWindowSize = 0;
  double sampleRateHz = 0.0;
  double nominalFrequencyHz = 50.0;
  std::uint8_t expectedSampleSynchronization = 0xff;
  std::array<std::uint16_t, 3> appIds{};
  std::vector<ProtocolRealtimeValueType> valueTypes;
};

enum class SvRealtimeProcessResult {
  ACCEPTED,
  DUPLICATE,
  CONFLICT,
  SEQUENCE_GAP,
  REJECTED,
};

struct SvRealtimeStatistics {
  std::uint64_t accepted = 0;
  std::uint64_t duplicate = 0;
  std::uint64_t conflict = 0;
  std::uint64_t rejected = 0;
  std::uint64_t sequenceGapEvents = 0;
  std::uint64_t missingSamples = 0;
  std::uint64_t busy = 0;
};

// SV实时状态引擎。构造后不再分配内存，处理阶段只复制固定标量值。
class SvRealtimeEngine {
public:
  explicit SvRealtimeEngine(
      std::vector<SvRealtimeSubscriptionConfig> subscriptions);

  SvRealtimeProcessResult TryProcess(const ProtocolSvFrameView& frame,
                                     std::size_t* acceptedRoute,
                                     std::uint32_t* missingSamples = nullptr) noexcept;
  std::span<const ProtocolRealtimeValue> values(
      std::size_t route) const noexcept;
  // 将指定成员的窗口按时间升序复制到调用方提供的固定缓冲区。
  // 返回值为实际样本数；BOOL成员、窗口未启用或参数无效时返回0。
  std::size_t CopyNumericSamples(std::size_t route, std::size_t member,
                                 std::span<double> output) const noexcept;
  const SvRealtimeStatistics& statistics() const noexcept;
  std::size_t size() const noexcept;

  bool TryLock() const noexcept;
  void Unlock() const noexcept;

private:
  struct SubscriptionState {
    struct RecentAsdu {
      bool valid = false;
      std::uint16_t sampleCount = 0;
      std::vector<ProtocolRealtimeValue> values;
    };

    SvRealtimeSubscriptionConfig config;
    std::vector<ProtocolRealtimeValue> values;
    std::vector<RecentAsdu> recentAsdus;
    std::size_t sampleWindowSize = 0;
    std::size_t sampleWindowWriteIndex = 0;
    std::size_t sampleWindowValueCount = 0;
    std::vector<double> sampleWindow;
    bool hasSampleCount = false;
    std::uint16_t sampleCount = 0;
    std::uint32_t asduIndex = 0;
  };

  SvRealtimeProcessResult Reject() noexcept;
  static bool EqualValue(const ProtocolRealtimeValue& left,
                         const ProtocolRealtimeValue& right) noexcept;

  std::vector<SubscriptionState> subscriptions_;
  mutable std::atomic_flag processing_ = ATOMIC_FLAG_INIT;
  SvRealtimeStatistics statistics_;
};

}  // namespace IEC61850
