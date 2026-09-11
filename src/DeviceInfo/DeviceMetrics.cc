#include "DeviceMetrics.hpp"

#include <charconv>
#include <limits>
#include <string_view>
#include <vector>

namespace DeviceInfo {
namespace {

bool ParseUnsigned(std::string_view text, uint64_t* value) {
  if (text.empty() || value == nullptr) {
    return false;
  }
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), *value);
  return error == std::errc{} && end == text.data() + text.size();
}

std::string_view NextToken(std::string_view* input) {
  const auto first = input->find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    *input = {};
    return {};
  }
  input->remove_prefix(first);
  const auto end = input->find_first_of(" \t\r");
  if (end == std::string_view::npos) {
    auto token = *input;
    *input = {};
    return token;
  }
  const auto token = input->substr(0, end);
  input->remove_prefix(end);
  return token;
}

}  // namespace

std::optional<CpuTimes> ParseCpuTimes(std::string_view content) {
  const auto lineEnd = content.find('\n');
  auto line = content.substr(0, lineEnd);
  if (NextToken(&line) != "cpu") {
    return std::nullopt;
  }

  std::vector<uint64_t> fields;
  while (!line.empty()) {
    uint64_t field = 0;
    if (!ParseUnsigned(NextToken(&line), &field)) {
      return std::nullopt;
    }
    fields.push_back(field);
  }
  if (fields.size() < 4) {
    return std::nullopt;
  }
  uint64_t total = 0;
  for (const auto field : fields) {
    if (field > std::numeric_limits<uint64_t>::max() - total) {
      return std::nullopt;
    }
    total += field;
  }
  uint64_t busy = 0;
  for (size_t index : {0U, 1U, 2U}) {
    if (fields[index] > std::numeric_limits<uint64_t>::max() - busy) {
      return std::nullopt;
    }
    busy += fields[index];
  }
  for (size_t index : {5U, 6U, 7U}) {
    if (index < fields.size()) {
      if (fields[index] > std::numeric_limits<uint64_t>::max() - busy) {
        return std::nullopt;
      }
      busy += fields[index];
    }
  }
  return CpuTimes{.busy = busy, .total = total};
}

std::optional<double> CalculateCpuUsage(const CpuTimes& previous,
                                        const CpuTimes& current) {
  if (current.total <= previous.total || current.busy < previous.busy) {
    return std::nullopt;
  }
  const auto totalDelta = current.total - previous.total;
  const auto busyDelta = current.busy - previous.busy;
  if (busyDelta > totalDelta) {
    return std::nullopt;
  }
  return static_cast<double>(busyDelta) * 100.0 /
         static_cast<double>(totalDelta);
}

std::optional<double> ParseMemoryUsage(std::string_view content) {
  std::optional<uint64_t> total;
  std::optional<uint64_t> available;
  while (!content.empty()) {
    const auto lineEnd = content.find('\n');
    const auto line = content.substr(0, lineEnd);
    if (lineEnd == std::string_view::npos) {
      content = {};
    } else {
      content.remove_prefix(lineEnd + 1);
    }

    const auto separator = line.find(':');
    if (separator == std::string_view::npos) {
      continue;
    }
    const auto name = line.substr(0, separator);
    auto valuePart = line.substr(separator + 1);
    uint64_t value = 0;
    if (!ParseUnsigned(NextToken(&valuePart), &value)) {
      if (name == "MemTotal" || name == "MemAvailable") {
        return std::nullopt;
      }
      continue;
    }
    const auto unit = NextToken(&valuePart);
    if (!unit.empty() && unit != "kB") {
      return std::nullopt;
    }
    if (name == "MemTotal") {
      total = value;
    } else if (name == "MemAvailable") {
      available = value;
    }
  }

  if (!total || !available || *total == 0 || *available > *total) {
    return std::nullopt;
  }
  return static_cast<double>(*total - *available) * 100.0 /
         static_cast<double>(*total);
}

std::optional<double> CpuUsageTracker::Update(const CpuTimes& current) {
  if (!previous_) {
    previous_ = current;
    return std::nullopt;
  }
  const auto result = CalculateCpuUsage(*previous_, current);
  previous_ = current;
  return result;
}

}  // namespace DeviceInfo
