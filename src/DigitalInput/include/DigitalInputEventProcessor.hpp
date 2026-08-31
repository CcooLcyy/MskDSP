#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <optional>
#include <array>
#include <string>
#include <string_view>

namespace DigitalInput {

struct GpioEvent {
  uint32_t offset = 0;
  bool physicalHigh = false;
  int64_t timestampMs = 0;
  bool sequenceGap = false;
};

struct PublishedDigitalInput {
  std::string tag;
  bool value = false;
  int64_t timestampMs = 0;
};

struct DigitalInputChannel {
  uint32_t offset;
  std::string_view tag;
};

inline constexpr std::array<DigitalInputChannel, 4> kChannels = {{
    {.offset = 114, .tag = "DI1"},
    {.offset = 116, .tag = "DI2"},
    {.offset = 113, .tag = "DI3"},
    {.offset = 115, .tag = "DI4"},
}};

std::optional<std::string_view> TagForOffset(uint32_t offset);
bool PhysicalLevelToLogical(bool physicalHigh);

class DigitalInputEventProcessor {
public:
  using PublishCallback = std::function<bool(const PublishedDigitalInput&)>;

  explicit DigitalInputEventProcessor(PublishCallback publish);

  // 处理一条 GPIO 边沿；返回 true 表示产生了一次 SOE 发布。
  bool HandleEvent(const GpioEvent& event);
  void Reset();

private:
  PublishCallback publish_;
  std::array<std::optional<bool>, kChannels.size()> lastValues_;
};

}  // namespace DigitalInput
