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

}  // namespace IEC104::detail
