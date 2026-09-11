#pragma once

#include <expected>
#include <utility>

#include "IEC61850.pb.h"
#include "mskdsp/Decimal20.hpp"

namespace IEC61850 {

struct PointEngineeringDecimal {
  mskdsp::numeric::Decimal20 scale;
  mskdsp::numeric::Decimal20 offset;
  mskdsp::numeric::Decimal20 deadband;
};

// 按十进制文本优先规则解析普通点倍率、偏移和死区。
inline std::expected<PointEngineeringDecimal, mskdsp::numeric::DecimalError>
ParsePointEngineeringDecimal(const IEC61850Proto::PointMapping& point) {
  auto scale = mskdsp::numeric::ParseConfiguredDecimal(
      point.scale_decimal(), point.scale());
  if (!scale.has_value()) {
    return std::unexpected(scale.error());
  }
  auto offset = mskdsp::numeric::ParseConfiguredDecimal(
      point.offset_decimal(), point.offset());
  if (!offset.has_value()) {
    return std::unexpected(offset.error());
  }
  auto deadband = mskdsp::numeric::ParseConfiguredDecimal(
      point.deadband_decimal(), point.deadband());
  if (!deadband.has_value()) {
    return std::unexpected(deadband.error());
  }
  return PointEngineeringDecimal{.scale = std::move(*scale),
                                 .offset = std::move(*offset),
                                 .deadband = std::move(*deadband)};
}

}  // namespace IEC61850
