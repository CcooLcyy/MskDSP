#pragma once

#include <cstdint>
#include <optional>

#include "DataCenter.pb.h"
#include "mskdsp/Decimal20.hpp"

namespace IEC104::detail {

struct LastReportedTelemetry {
  mskdsp::numeric::Decimal20 value;
  DataCenterProto::Quality quality = DataCenterProto::QUALITY_UNSPECIFIED;
  int64_t tsMs = 0;
};

struct LastReportedSinglePoint {
  bool value = false;
  DataCenterProto::Quality quality = DataCenterProto::QUALITY_UNSPECIFIED;
};

inline bool ShouldReportTelemetry(const mskdsp::numeric::Decimal20 &value,
                                  const mskdsp::numeric::Decimal20 &deadband,
                                  DataCenterProto::Quality quality,
                                  int64_t tsMs,
                                  const std::optional<LastReportedTelemetry> &last) {
  if (deadband <= mskdsp::numeric::Decimal20{} || !last.has_value() ||
      quality != last->quality || tsMs != last->tsMs) {
    return true;
  }
  return mskdsp::numeric::ShouldReport(value, last->value, deadband);
}

inline bool ShouldReportTelemetry(double value,
                                  double deadband,
                                  DataCenterProto::Quality quality,
                                  int64_t tsMs,
                                  const std::optional<LastReportedTelemetry> &last) {
  auto decimalDeadband = mskdsp::numeric::Decimal20::FromDouble(deadband);
  if (!decimalDeadband) {
    return true;
  }
  auto decimalValue = mskdsp::numeric::Decimal20::FromDouble(value);
  if (!decimalValue) {
    return true;
  }
  return ShouldReportTelemetry(*decimalValue, *decimalDeadband, quality, tsMs,
                               last);
}

// 遥信 SOE 变位判定：只有状态或品质发生变化（或该点尚无历史基线）时才形成新事件。
// 时标不参与判定：上游每次刷新时标不构成变位，否则同值重复发布会被放大成 SOE 风暴。
inline bool ShouldReportSoe(bool value,
                            DataCenterProto::Quality quality,
                            const std::optional<LastReportedSinglePoint> &last) {
  if (!last.has_value()) {
    return true;
  }
  return value != last->value || quality != last->quality;
}

}  // namespace IEC104::detail
