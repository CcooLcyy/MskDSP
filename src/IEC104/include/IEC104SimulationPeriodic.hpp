#pragma once

#include <chrono>
#include <cstdint>

namespace IEC104::detail {

// 仅负责单连接的周期计划；由管理器互斥量保护，显式传入时钟便于确定性验证。
class SimulationPeriodicSchedule {
public:
  using Clock = std::chrono::steady_clock;

  bool Configure(bool enabled, uint32_t intervalSeconds, Clock::time_point now) {
    if (enabled_ == enabled && intervalSeconds_ == intervalSeconds) {
      return false;
    }
    enabled_ = enabled;
    intervalSeconds_ = intervalSeconds;
    nextDue_ = now + std::chrono::seconds(intervalSeconds_);
    return true;
  }

  void Disable() {
    enabled_ = false;
  }
  bool enabled() const {
    return enabled_;
  }
  uint32_t intervalSeconds() const {
    return intervalSeconds_;
  }
  int64_t lastSentMs() const {
    return lastSentMs_;
  }
  void MarkSent(int64_t timestampMs) {
    lastSentMs_ = timestampMs;
  }

  bool ConsumeDue(Clock::time_point now, bool ready) {
    if (!enabled_ || now < nextDue_) {
      return false;
    }
    // 暂停或调度延迟时同样推进，不追补错过的周期，避免恢复后的突发发送。
    nextDue_ = now + std::chrono::seconds(intervalSeconds_);
    return ready;
  }

private:
  bool enabled_ = false;
  uint32_t intervalSeconds_ = 5;
  int64_t lastSentMs_ = 0;
  Clock::time_point nextDue_{};
};

}  // namespace IEC104::detail
