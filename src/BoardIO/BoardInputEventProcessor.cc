#include "BoardInputEventProcessor.hpp"

#include <utility>

namespace BoardIO {

std::optional<std::string_view> TagForOffset(uint32_t offset) {
  for (const auto& channel : kChannels) {
    if (channel.offset == offset) {
      return channel.tag;
    }
  }
  return std::nullopt;
}

bool PhysicalLevelToLogical(bool physicalHigh) {
  // 外部 DI 短接时为物理低电平，业务上定义为有效 true。
  return !physicalHigh;
}

BoardInputEventProcessor::BoardInputEventProcessor(PublishCallback publish) :
  publish_(std::move(publish)) {}

bool BoardInputEventProcessor::HandleEvent(const GpioEvent& event) {
  const auto tag = TagForOffset(event.offset);
  if (!tag.has_value()) {
    return false;
  }

  std::size_t channelIndex = 0;
  for (; channelIndex < kChannels.size(); ++channelIndex) {
    if (kChannels[channelIndex].offset == event.offset) {
      break;
    }
  }
  if (channelIndex >= lastValues_.size()) {
    return false;
  }

  const bool logicalValue = PhysicalLevelToLogical(event.physicalHigh);
  if (lastValues_[channelIndex].has_value() &&
      *lastValues_[channelIndex] == logicalValue) {
    return false;
  }

  if (publish_) {
    const bool published = publish_(PublishedBoardInput{
        .tag = std::string(*tag),
        .value = logicalValue,
        .timestampMs = event.timestampMs,
    });
    if (!published) {
      return false;
    }
  }
  lastValues_[channelIndex] = logicalValue;
  return true;
}

void BoardInputEventProcessor::Reset() {
  for (auto& value : lastValues_) {
    value.reset();
  }
}

}  // namespace BoardIO
