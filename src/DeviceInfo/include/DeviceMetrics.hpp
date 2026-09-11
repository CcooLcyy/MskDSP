#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace DeviceInfo {

struct CpuTimes {
  uint64_t busy = 0;
  uint64_t total = 0;
};

std::optional<CpuTimes> ParseCpuTimes(std::string_view content);
std::optional<double> CalculateCpuUsage(const CpuTimes& previous,
                                        const CpuTimes& current);
std::optional<double> ParseMemoryUsage(std::string_view content);

class CpuUsageTracker {
public:
  std::optional<double> Update(const CpuTimes& current);

private:
  std::optional<CpuTimes> previous_;
};

}  // namespace DeviceInfo
