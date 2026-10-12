#pragma once

#include <cstdint>
#include <optional>

namespace ModbusRTU {

// raw 已按寄存器字节序/字序归一化；点表验证保证枚举值与位索引二选一且位索引小于 32。
inline bool DecodeRegisterBool(uint32_t raw, std::optional<uint32_t> bitIndex, std::optional<uint32_t> enumValue) {
  if (enumValue.has_value()) {
    return raw == enumValue.value();
  }
  return ((raw >> bitIndex.value()) & 1u) != 0;
}

}  // 命名空间结束
