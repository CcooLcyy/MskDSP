#pragma once

#include <optional>

#include "IEC104PointTable.h"
#include "IEC104TcpLink.h"

namespace IEC104::detail {

// 所有从站发送路径共用固定报文值构造，不经过工程量或模拟值换算。
inline std::optional<PointValue> MakeFixedPointValue(const PointTable::Point& point,
                                                   int64_t nowMs) {
  if (!point.fixedValueEnabled) {
    return std::nullopt;
  }
  PointValue value;
  value.ioa = point.ioa;
  value.type = point.type;
  value.quality = 0;
  value.tsMs = nowMs;
  if (point.type == IEC104Proto::POINT_TYPE_FLOAT) {
    value.doubleValue = static_cast<double>(static_cast<float>(point.fixedValue));
  } else if (point.type == IEC104Proto::POINT_TYPE_SINGLE) {
    value.boolValue = point.fixedValue != 0;
  } else {
    return std::nullopt;
  }
  return value;
}

}  // IEC104 固定值辅助命名空间结束
