#include "IEC61850MmsChannelPolicy.h"

#include <algorithm>
#include <ranges>

namespace IEC61850 {

bool ShouldScheduleRcbReconfigurationAfterConnect(
    IEC61850Proto::NetworkChannel channel,
    std::span<const MmsChannelStatus> channels,
    std::optional<IEC61850Proto::NetworkChannel> configurationChannel) {
  if (channel == IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED ||
      configurationChannel.has_value()) {
    return false;
  }
  return std::ranges::any_of(channels, [&](const auto& status) {
    if (status.channel == IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED ||
        status.channel >= channel) {
      return false;
    }
    return status.state != IEC61850Proto::CHANNEL_STATE_CONNECTING &&
           status.state != IEC61850Proto::CHANNEL_STATE_CONNECTED;
  });
}

}  // namespace IEC61850
