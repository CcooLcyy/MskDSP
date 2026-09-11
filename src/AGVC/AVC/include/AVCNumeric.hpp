#pragma once

#include <expected>
#include <string_view>

#include "AVC.pb.h"
#include "mskdsp/Decimal20.hpp"

namespace AVC::numeric {

using Decimal = mskdsp::numeric::Decimal20;
using DecimalError = mskdsp::numeric::DecimalError;

inline const Decimal& Zero() {
  static const Decimal value;
  return value;
}

inline const Decimal& One() {
  static const Decimal value = Decimal::FromInt64(1).value();
  return value;
}

inline std::expected<Decimal, DecimalError> Configured(
    std::string_view decimalText, double legacyValue) {
  return mskdsp::numeric::ParseConfiguredDecimal(decimalText, legacyValue);
}

inline std::expected<Decimal, DecimalError> Scale(
    const AVCProto::SignalSpec& signal) {
  auto value = Configured(signal.scale_decimal(), signal.scale());
  if (value.has_value() && value->IsZero()) {
    return One();
  }
  return value;
}

inline std::expected<Decimal, DecimalError> Offset(
    const AVCProto::SignalSpec& signal) {
  return Configured(signal.offset_decimal(), signal.offset());
}

inline std::expected<Decimal, DecimalError> Kp(
    const AVCProto::VoltageControlConfig& config) {
  return Configured(config.kp_decimal(), config.kp());
}

inline std::expected<Decimal, DecimalError> Deadband(
    const AVCProto::VoltageControlConfig& config) {
  return Configured(config.deadband_decimal(), config.deadband());
}

inline std::expected<Decimal, DecimalError> Weight(
    const AVCProto::MemberConfig& member) {
  return Configured(member.weight_decimal(), member.weight());
}

inline std::expected<Decimal, DecimalError> Minimum(
    const AVCProto::MemberConfig& member) {
  return Configured(member.q_min_kvar_decimal(), member.q_min_kvar());
}

inline std::expected<Decimal, DecimalError> Maximum(
    const AVCProto::MemberConfig& member) {
  return Configured(member.q_max_kvar_decimal(), member.q_max_kvar());
}

inline double ToLegacyDouble(const Decimal& value) {
  return value.ToDouble().value_or(0.0);
}

inline std::expected<Decimal, DecimalError> Negate(const Decimal& value) {
  return Zero().Subtract(value);
}

inline std::expected<Decimal, DecimalError> Absolute(const Decimal& value) {
  return value < Zero() ? Negate(value)
                        : std::expected<Decimal, DecimalError>(value);
}

}  // 命名空间 AVC::numeric
