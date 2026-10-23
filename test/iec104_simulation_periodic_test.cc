#include <gtest/gtest.h>

#include <chrono>

#include "IEC104SimulationPeriodic.hpp"

namespace {
using IEC104::detail::SimulationPeriodicSchedule;
using namespace std::chrono_literals;
const auto kStart = SimulationPeriodicSchedule::Clock::time_point{};
}  // namespace

// 验证默认关闭，首次开启必须等待完整周期，相同设置不重置计时。
TEST(IEC104SimulationPeriodicTest, DefaultsAndIdempotentEnable) {
  SimulationPeriodicSchedule schedule;
  EXPECT_FALSE(schedule.enabled());
  EXPECT_EQ(schedule.intervalSeconds(), 5u);
  EXPECT_EQ(schedule.lastSentMs(), 0);
  EXPECT_FALSE(schedule.ConsumeDue(kStart + 100s, true));
  EXPECT_TRUE(schedule.Configure(true, 5, kStart));
  EXPECT_FALSE(schedule.ConsumeDue(kStart + 4s, true));
  EXPECT_FALSE(schedule.Configure(true, 5, kStart + 4s));
  EXPECT_TRUE(schedule.ConsumeDue(kStart + 5s, true));
  EXPECT_FALSE(schedule.ConsumeDue(kStart + 5s, true));
}

// 验证更改间隔重新计时，关闭阻止后续发送并保留最近发送时间。
TEST(IEC104SimulationPeriodicTest, IntervalChangeDisableAndTimestamp) {
  SimulationPeriodicSchedule schedule;
  schedule.Configure(true, 5, kStart);
  schedule.MarkSent(1234);
  EXPECT_TRUE(schedule.Configure(true, 2, kStart + 4s));
  EXPECT_FALSE(schedule.ConsumeDue(kStart + 5s, true));
  EXPECT_TRUE(schedule.ConsumeDue(kStart + 6s, true));
  schedule.Disable();
  EXPECT_FALSE(schedule.ConsumeDue(kStart + 100s, true));
  EXPECT_EQ(schedule.lastSentMs(), 1234);
}

// 验证断线暂停但仍消费到期计划，恢复后不追补错过的批次。
TEST(IEC104SimulationPeriodicTest, PauseAndResumeWithoutCatchUp) {
  SimulationPeriodicSchedule schedule;
  schedule.Configure(true, 5, kStart);
  EXPECT_FALSE(schedule.ConsumeDue(kStart + 5s, false));
  EXPECT_FALSE(schedule.ConsumeDue(kStart + 8s, true));
  EXPECT_TRUE(schedule.ConsumeDue(kStart + 10s, true));
  EXPECT_TRUE(schedule.ConsumeDue(kStart + 100s, true));
  EXPECT_FALSE(schedule.ConsumeDue(kStart + 100s, true));
  EXPECT_FALSE(schedule.ConsumeDue(kStart + 104s, true));
}

// 验证多个连接的计划和最近发送时间相互独立。
TEST(IEC104SimulationPeriodicTest, IndependentConnections) {
  SimulationPeriodicSchedule first;
  SimulationPeriodicSchedule second;
  first.Configure(true, 1, kStart);
  second.Configure(true, 5, kStart);
  EXPECT_TRUE(first.ConsumeDue(kStart + 1s, true));
  EXPECT_FALSE(second.ConsumeDue(kStart + 1s, true));
  first.MarkSent(42);
  EXPECT_EQ(second.lastSentMs(), 0);
  first.Disable();
  EXPECT_TRUE(second.ConsumeDue(kStart + 5s, true));
}
