#pragma once

#include <cstdint>

namespace mskdsp::communication {
// 协议连接在 DataCenter 中注册的保留系统标签，普通点表不得占用。
inline constexpr char kStateTag[] = "__communication_state";

// 仅 Healthy 允许参与控制；本状态不保证全部点读取或控制写入成功。
enum class State : uint16_t {
  Unknown = 0,
  Healthy = 1,
  Unhealthy = 2,
  Stopped = 3,
};
}  // namespace mskdsp::communication
