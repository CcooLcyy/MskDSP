#include "DeviceMetrics.hpp"

#include <gtest/gtest.h>

namespace DeviceInfo {

// 验证 CPU 首次采样只建立基线，不产生占用率。
TEST(DeviceMetricsTest, FirstCpuSampleOnlyInitializesBaseline) {
  CpuUsageTracker tracker;
  ASSERT_FALSE(tracker.Update(CpuTimes{.busy = 10, .total = 20}).has_value());
}

// 验证 CPU 累计计数增量可以换算为百分比。
TEST(DeviceMetricsTest, CalculatesCpuUsageFromDelta) {
  const auto previous = CpuTimes{.busy = 40, .total = 100};
  const auto current = CpuTimes{.busy = 70, .total = 150};
  ASSERT_NE(CalculateCpuUsage(previous, current), std::nullopt);
  EXPECT_DOUBLE_EQ(*CalculateCpuUsage(previous, current), 60.0);
}

// 验证 CPU 计数器回退或无增量时拒绝发布结果。
TEST(DeviceMetricsTest, RejectsInvalidCpuDelta) {
  EXPECT_FALSE(CalculateCpuUsage(CpuTimes{10, 100}, CpuTimes{9, 110}).has_value());
  EXPECT_FALSE(CalculateCpuUsage(CpuTimes{10, 100}, CpuTimes{10, 100}).has_value());
  EXPECT_FALSE(CalculateCpuUsage(CpuTimes{10, 100}, CpuTimes{120, 110}).has_value());
}

// 验证 /proc/stat 总 CPU 行解析并忽略后续核心行。
TEST(DeviceMetricsTest, ParsesTotalCpuLine) {
  const auto result = ParseCpuTimes("cpu  10 2 8 70 5 1 2 3\ncpu0 1 0 1 8\n");
  ASSERT_NE(result, std::nullopt);
  EXPECT_EQ(result->busy, 26U);
  EXPECT_EQ(result->total, 101U);
}

// 验证 MemTotal 与 MemAvailable 可以计算内存占用率。
TEST(DeviceMetricsTest, CalculatesMemoryUsage) {
  const auto result = ParseMemoryUsage("MemTotal:       1000 kB\nMemAvailable:    250 kB\n");
  ASSERT_NE(result, std::nullopt);
  EXPECT_DOUBLE_EQ(*result, 75.0);
}

// 验证内存字段缺失、非法和越界时返回无效结果。
TEST(DeviceMetricsTest, RejectsInvalidMemoryInput) {
  EXPECT_FALSE(ParseMemoryUsage("MemTotal: 1000 kB\n").has_value());
  EXPECT_FALSE(ParseMemoryUsage("MemTotal: bad kB\nMemAvailable: 100 kB\n").has_value());
  EXPECT_FALSE(ParseMemoryUsage("MemTotal: 0 kB\nMemAvailable: 0 kB\n").has_value());
  EXPECT_FALSE(ParseMemoryUsage("MemTotal: 100 kB\nMemAvailable: 200 kB\n").has_value());
}

}  // namespace DeviceInfo
