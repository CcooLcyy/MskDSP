#pragma once

#include <optional>
#include <span>

#include "IEC61850ProtocolStack.h"

namespace IEC61850 {

// 判断通道刚完成建链时是否应主动重建会话，以接管遗漏的RCB配置。
// 只有较低编号通道已经明确失败、当前没有配置权时才返回true。
bool ShouldScheduleRcbReconfigurationAfterConnect(
    IEC61850Proto::NetworkChannel channel,
    std::span<const MmsChannelStatus> channels,
    std::optional<IEC61850Proto::NetworkChannel> configurationChannel);

}  // namespace IEC61850
