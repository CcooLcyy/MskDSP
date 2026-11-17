#pragma once

namespace AGVC {

// 两种控制组使用相同的协议数值。模式名称与已有配置的字符串解析相互独立。
template <typename ProtocolMode>
inline constexpr ProtocolMode kEventTriggered = static_cast<ProtocolMode>(1);

template <typename ProtocolMode>
inline constexpr ProtocolMode kPeriodicAllocation = static_cast<ProtocolMode>(2);

}  // namespace AGVC
