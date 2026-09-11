#pragma once

#include <expected>
#include <string_view>

#include "AGC.pb.h"
#include "mskdsp/Decimal20.hpp"

namespace AGC::numeric {

using Decimal = mskdsp::numeric::Decimal20;
using DecimalError = mskdsp::numeric::DecimalError;

inline const Decimal &Zero() {
  static const Decimal value;
  return value;
}

inline const Decimal &One() {
  static const Decimal value = Decimal::FromInt64(1).value();
  return value;
}

inline std::expected<Decimal, DecimalError> Configured(
    std::string_view decimalText, double legacyValue) {
  return mskdsp::numeric::ParseConfiguredDecimal(decimalText, legacyValue);
}

inline std::expected<Decimal, DecimalError> Scale(
    const AGCProto::SignalSpec &signal) {
  auto value = Configured(signal.scale_decimal(), signal.scale());
  if (value.has_value() && value->IsZero()) {
    return One();
  }
  return value;
}

inline std::expected<Decimal, DecimalError> Offset(
    const AGCProto::SignalSpec &signal) {
  return Configured(signal.offset_decimal(), signal.offset());
}

inline std::expected<Decimal, DecimalError> Capacity(
    const AGCProto::MemberConfig &member) {
  return Configured(member.capacity_kw_decimal(), member.capacity_kw());
}

inline std::expected<Decimal, DecimalError> Weight(
    const AGCProto::MemberConfig &member) {
  return Configured(member.weight_decimal(), member.weight());
}

inline std::expected<Decimal, DecimalError> Minimum(
    const AGCProto::MemberConfig &member) {
  return Configured(member.min_kw_decimal(), member.min_kw());
}

inline std::expected<Decimal, DecimalError> Maximum(
    const AGCProto::MemberConfig &member) {
  return Configured(member.max_kw_decimal(), member.max_kw());
}

inline std::expected<Decimal, DecimalError> UpPGain(
    const AGCProto::MemberControlProfile &profile) {
  return Configured(profile.up_p_gain_decimal(), profile.up_p_gain());
}

inline std::expected<Decimal, DecimalError> UpIGain(
    const AGCProto::MemberControlProfile &profile) {
  return Configured(profile.up_i_gain_decimal(), profile.up_i_gain());
}

inline std::expected<Decimal, DecimalError> DownPGain(
    const AGCProto::MemberControlProfile &profile) {
  return Configured(profile.down_p_gain_decimal(), profile.down_p_gain());
}

inline std::expected<Decimal, DecimalError> DownIGain(
    const AGCProto::MemberControlProfile &profile) {
  return Configured(profile.down_i_gain_decimal(), profile.down_i_gain());
}

inline std::expected<Decimal, DecimalError> UpBias(
    const AGCProto::MemberControlProfile &profile) {
  return Configured(profile.up_bias_kw_decimal(), profile.up_bias_kw());
}

inline std::expected<Decimal, DecimalError> DownBias(
    const AGCProto::MemberControlProfile &profile) {
  return Configured(profile.down_bias_kw_decimal(), profile.down_bias_kw());
}

inline std::expected<Decimal, DecimalError> IntegralLimit(
    const AGCProto::MemberControlProfile &profile) {
  return Configured(profile.integral_limit_kw_decimal(),
                    profile.integral_limit_kw());
}

inline std::expected<Decimal, DecimalError> MaximumStep(
    const AGCProto::MemberControlProfile &profile) {
  return Configured(profile.max_step_kw_decimal(), profile.max_step_kw());
}

inline std::expected<Decimal, DecimalError> MaximumRamp(
    const AGCProto::MemberControlProfile &profile) {
  return Configured(profile.max_ramp_kw_per_s_decimal(),
                    profile.max_ramp_kw_per_s());
}

inline std::expected<Decimal, DecimalError> TuningLower(
    const AGCProto::TuningConfig &config) {
  return Configured(config.target_lower_kw_decimal(), config.target_lower_kw());
}

inline std::expected<Decimal, DecimalError> TuningUpper(
    const AGCProto::TuningConfig &config) {
  return Configured(config.target_upper_kw_decimal(), config.target_upper_kw());
}

inline std::expected<Decimal, DecimalError> TuningTolerance(
    const AGCProto::TuningConfig &config) {
  return Configured(config.total_tolerance_kw_decimal(),
                    config.total_tolerance_kw());
}

inline double ToLegacyDouble(const Decimal &value) {
  return value.ToDouble().value_or(0.0);
}

inline std::expected<Decimal, DecimalError> Negate(const Decimal &value) {
  return Zero().Subtract(value);
}

inline std::expected<Decimal, DecimalError> Absolute(const Decimal &value) {
  return value < Zero() ? Negate(value)
                        : std::expected<Decimal, DecimalError>(value);
}

}  // namespace AGC::numeric
