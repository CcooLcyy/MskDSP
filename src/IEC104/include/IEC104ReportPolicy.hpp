#pragma once

#include <cstdint>
#include <optional>

#include "DataCenter.pb.h"
#include "mskdsp/Decimal20.hpp"

namespace IEC104::detail {

inline constexpr uint8_t kSinglePointQualityMask = 0xF0;
inline constexpr uint8_t kMeasuredValueQualityMask = 0xF1;

inline std::optional<uint8_t> ExplicitWireQuality(const DataCenterProto::PointUpdate &update,
                                                  uint8_t mask) {
  if (!update.has_quality_flags()) {
    return std::nullopt;
  }
  return static_cast<uint8_t>(update.quality_flags() & mask);
}

inline uint8_t WireQuality(DataCenterProto::Quality quality,
                           std::optional<uint8_t> explicitQuality) {
  return explicitQuality.value_or(quality == DataCenterProto::QUALITY_GOOD ? 0x00 : 0x80);
}

inline bool ReportQualityChanged(DataCenterProto::Quality quality,
                                 std::optional<uint8_t> wireQuality,
                                 DataCenterProto::Quality lastQuality,
                                 std::optional<uint8_t> lastWireQuality) {
  // 两次均来自旧接口时，保留原有四级品质变化的判定。
  if (!wireQuality && !lastWireQuality) {
    return quality != lastQuality;
  }
  return WireQuality(quality, wireQuality) != WireQuality(lastQuality, lastWireQuality);
}

struct LastReportedTelemetry {
  mskdsp::numeric::Decimal20 value;
  DataCenterProto::Quality quality = DataCenterProto::QUALITY_UNSPECIFIED;
  int64_t tsMs = 0;
  std::optional<uint8_t> wireQuality = std::nullopt;
};

struct LastReportedSinglePoint {
  bool value = false;
  DataCenterProto::Quality quality = DataCenterProto::QUALITY_UNSPECIFIED;
  std::optional<uint8_t> wireQuality = std::nullopt;
};

inline bool ShouldReportTelemetry(const mskdsp::numeric::Decimal20 &value,
                                  const mskdsp::numeric::Decimal20 &deadband,
                                  DataCenterProto::Quality quality,
                                  int64_t tsMs,
                                  const std::optional<LastReportedTelemetry> &last,
                                  std::optional<uint8_t> wireQuality = std::nullopt) {
  if (deadband <= mskdsp::numeric::Decimal20{} || !last.has_value() ||
      ReportQualityChanged(quality, wireQuality, last->quality, last->wireQuality) ||
      tsMs != last->tsMs) {
    return true;
  }
  return mskdsp::numeric::ShouldReport(value, last->value, deadband);
}

inline bool ShouldReportTelemetry(double value,
                                  double deadband,
                                  DataCenterProto::Quality quality,
                                  int64_t tsMs,
                                  const std::optional<LastReportedTelemetry> &last,
                                  std::optional<uint8_t> wireQuality = std::nullopt) {
  auto decimalDeadband = mskdsp::numeric::Decimal20::FromDouble(deadband);
  if (!decimalDeadband) {
    return true;
  }
  auto decimalValue = mskdsp::numeric::Decimal20::FromDouble(value);
  if (!decimalValue) {
    return true;
  }
  return ShouldReportTelemetry(*decimalValue, *decimalDeadband, quality, tsMs,
                               last, wireQuality);
}

// 遥信 SOE 变位判定：只有状态或品质发生变化（或该点尚无历史基线）时才形成新事件。
// 时标不参与判定：上游每次刷新时标不构成变位，否则同值重复发布会被放大成 SOE 风暴。
inline bool ShouldReportSoe(bool value,
                            DataCenterProto::Quality quality,
                            const std::optional<LastReportedSinglePoint> &last,
                            std::optional<uint8_t> wireQuality = std::nullopt) {
  if (!last.has_value()) {
    return true;
  }
  return value != last->value ||
      ReportQualityChanged(quality, wireQuality, last->quality, last->wireQuality);
}

}  // namespace IEC104::detail
