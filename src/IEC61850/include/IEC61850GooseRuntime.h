#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "IEC61850ProtocolStack.h"

namespace IEC61850 {

struct GooseRealtimeSubscriptionConfig {
  std::uint32_t subscriptionId = 0;
  std::string gocbRef;
  std::string dataSetRef;
  std::string goId;
  std::uint64_t configRevision = 0;
  std::array<std::uint16_t, 3> appIds{};
  std::vector<std::uint32_t> signalIds;
  std::vector<ProtocolRealtimeValueType> valueTypes;
};

enum class GooseRealtimeProcessResult {
  ACCEPTED,
  // 输入已从TTL超时恢复；管理器需要重新发布一次当前值以清除通信无效。
  RECOVERED,
  DUPLICATE,
  CONFLICT,
  REJECTED,
  TIMED_OUT,
  NO_CHANGE,
};

enum class GooseRealtimeInputState {
  NOT_RECEIVED,
  ACTIVE,
  TIMED_OUT,
};

struct GooseRealtimeStatistics {
  std::uint64_t accepted = 0;
  std::uint64_t duplicate = 0;
  std::uint64_t conflict = 0;
  std::uint64_t rejected = 0;
  std::uint64_t timedOut = 0;
  std::uint64_t busy = 0;
};

// GOOSE实时状态引擎。构造后不再分配内存，TryProcess并发时直接拒绝竞争者。
class GooseRealtimeEngine {
public:
  explicit GooseRealtimeEngine(
      std::vector<GooseRealtimeSubscriptionConfig> subscriptions);

  GooseRealtimeProcessResult TryProcess(const ProtocolGooseFrameView& frame,
                                         std::int64_t nowNs,
                                         std::size_t* acceptedRoute) noexcept;
  GooseRealtimeProcessResult CheckTimeout(std::int64_t nowNs,
                                          std::size_t* timedOutRoute,
                                          // 在引擎锁内复制超时前的最后值；
                                          // 缓冲不足时只报告所需数量，不写入部分快照。
                                          std::span<ProtocolRealtimeValue>
                                              timedOutValues = {},
                                          std::size_t* timedOutValueCount =
                                              nullptr) noexcept;

  // 仅用于初始化和诊断；实时超时路径必须使用CheckTimeout返回的快照。
  std::span<const ProtocolRealtimeValue> values(std::size_t route) const noexcept;
  std::span<const std::uint32_t> signalIds(std::size_t route) const noexcept;
  GooseRealtimeInputState state(std::size_t route) const noexcept;
  const GooseRealtimeStatistics& statistics() const noexcept;
  std::size_t size() const noexcept;

  // 仅供协议栈回调保护临界区使用，不执行阻塞操作。
  bool TryLock() noexcept;
  void Unlock() noexcept;

private:
  struct SubscriptionState {
    GooseRealtimeSubscriptionConfig config;
    GooseRealtimeInputState state = GooseRealtimeInputState::NOT_RECEIVED;
    bool hasSequence = false;
    std::uint32_t stateNumber = 0;
    std::uint32_t sequenceNumber = 0;
    std::int64_t expiresAtNs = 0;
    std::vector<ProtocolRealtimeValue> values;
  };

  static std::int64_t SaturatingExpiry(std::int64_t nowNs,
                                       std::uint32_t ttlMs) noexcept;
  static bool EqualValue(const ProtocolRealtimeValue& left,
                         const ProtocolRealtimeValue& right) noexcept;
  GooseRealtimeProcessResult Reject() noexcept;

  std::vector<SubscriptionState> subscriptions_;
  std::atomic_flag processing_ = ATOMIC_FLAG_INIT;
  GooseRealtimeStatistics statistics_;
};

}  // namespace IEC61850
