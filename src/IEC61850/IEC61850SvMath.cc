#include "IEC61850SvMath.h"

#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace IEC61850 {
namespace {

template <typename T>
SvMathResult<T> Failure(SvMathStatus status) noexcept {
  return SvMathResult<T>{.value = std::nullopt, .status = status};
}

template <typename T>
SvMathResult<T> Success(T value) noexcept {
  return SvMathResult<T>{.value = std::move(value), .status = SvMathStatus::OK};
}

bool IsFinite(double value) noexcept { return std::isfinite(value); }

bool ValidateSampleRate(double sampleRateHz) noexcept {
  return IsFinite(sampleRateHz) && sampleRateHz > 0.0;
}

bool AllFinite(std::span<const double> values) noexcept {
  for (const double value : values) {
    if (!IsFinite(value)) {
      return false;
    }
  }
  return true;
}

}  // namespace

const char* ToString(SvMathStatus status) noexcept {
  switch (status) {
    case SvMathStatus::OK:
      return "成功";
    case SvMathStatus::WINDOW_TOO_SHORT:
      return "窗口长度不足";
    case SvMathStatus::NON_FINITE_INPUT:
      return "输入包含非有限值";
    case SvMathStatus::INVALID_INPUT:
      return "输入参数非法";
    case SvMathStatus::INVALID_SAMPLE_RATE:
      return "采样率非法";
    case SvMathStatus::INVALID_FREQUENCY:
      return "频率参数非法";
    case SvMathStatus::INPUT_SIZE_MISMATCH:
      return "输入窗口长度不一致";
    case SvMathStatus::INSUFFICIENT_CROSSINGS:
      return "上升过零点不足";
    case SvMathStatus::INSUFFICIENT_PHASE_PROGRESS:
      return "相位推进不足";
    case SvMathStatus::NUMERICAL_ERROR:
      return "数值计算溢出或非有限";
  }
  return "未知数学状态";
}

SvMathResult<double> ComputeRms(std::span<const double> samples) noexcept {
  if (samples.empty()) {
    return Failure<double>(SvMathStatus::WINDOW_TOO_SHORT);
  }
  if (!AllFinite(samples)) {
    return Failure<double>(SvMathStatus::NON_FINITE_INPUT);
  }

  long double sumSquares = 0.0L;
  for (const double sample : samples) {
    const long double value = static_cast<long double>(sample);
    sumSquares += value * value;
  }
  const long double meanSquare =
      sumSquares / static_cast<long double>(samples.size());
  const long double rms = std::sqrt(meanSquare);
  const double result = static_cast<double>(rms);
  if (!std::isfinite(rms) || !IsFinite(result)) {
    return Failure<double>(SvMathStatus::NUMERICAL_ERROR);
  }
  return Success(result);
}

SvMathResult<SvPhasor> ComputeSingleFrequencyPhasor(
    std::span<const double> samples, double sampleRateHz,
    double frequencyHz) noexcept {
  if (!ValidateSampleRate(sampleRateHz)) {
    return Failure<SvPhasor>(SvMathStatus::INVALID_SAMPLE_RATE);
  }
  if (!IsFinite(frequencyHz) || frequencyHz <= 0.0 ||
      frequencyHz >= sampleRateHz / 2.0) {
    return Failure<SvPhasor>(SvMathStatus::INVALID_FREQUENCY);
  }
  const double samplesPerCycle = sampleRateHz / frequencyHz;
  if (samples.size() < 2 ||
      static_cast<double>(samples.size()) + std::numeric_limits<double>::epsilon() <
          std::ceil(samplesPerCycle)) {
    return Failure<SvPhasor>(SvMathStatus::WINDOW_TOO_SHORT);
  }
  if (!AllFinite(samples)) {
    return Failure<SvPhasor>(SvMathStatus::NON_FINITE_INPUT);
  }

  const long double angularStep =
      2.0L * std::numbers::pi_v<long double> * frequencyHz / sampleRateHz;
  long double realAccumulator = 0.0L;
  long double imaginaryAccumulator = 0.0L;
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const long double angle = angularStep * static_cast<long double>(index);
    const long double value = static_cast<long double>(samples[index]);
    realAccumulator += value * std::cos(angle);
    // 对 e^-jωt DFT 取负号，使相角与 cos(ωt+φ) 中的 φ 同号。
    imaginaryAccumulator -= value * std::sin(angle);
  }

  const long double rmsScale =
      std::sqrt(2.0L) / static_cast<long double>(samples.size());
  const double real = static_cast<double>(realAccumulator * rmsScale);
  const double imaginary = static_cast<double>(imaginaryAccumulator * rmsScale);
  if (!IsFinite(real) || !IsFinite(imaginary)) {
    return Failure<SvPhasor>(SvMathStatus::NUMERICAL_ERROR);
  }

  const double magnitude = std::hypot(real, imaginary);
  const double phase = std::atan2(imaginary, real);
  if (!IsFinite(magnitude) || !IsFinite(phase)) {
    return Failure<SvPhasor>(SvMathStatus::NUMERICAL_ERROR);
  }
  return Success(SvPhasor{.real = real,
                          .imaginary = imaginary,
                          .magnitude = magnitude,
                          .phaseRadians = phase});
}

SvMathResult<double> EstimateFrequencyByZeroCrossing(
    std::span<const double> samples, double sampleRateHz) noexcept {
  if (!ValidateSampleRate(sampleRateHz)) {
    return Failure<double>(SvMathStatus::INVALID_SAMPLE_RATE);
  }
  if (samples.size() < 3) {
    return Failure<double>(SvMathStatus::WINDOW_TOO_SHORT);
  }
  if (!AllFinite(samples)) {
    return Failure<double>(SvMathStatus::NON_FINITE_INPUT);
  }

  std::size_t crossingCount = 0;
  double firstCrossing = 0.0;
  double lastCrossing = 0.0;
  for (std::size_t index = 0; index + 1 < samples.size(); ++index) {
    const double left = samples[index];
    const double right = samples[index + 1];
    if (left <= 0.0 && right > 0.0) {
      const double denominator = right - left;
      if (!IsFinite(denominator) || denominator <= 0.0) {
        continue;
      }
      const double interpolation = -left / denominator;
      const double crossing = static_cast<double>(index) + interpolation;
      if (!IsFinite(crossing)) {
        return Failure<double>(SvMathStatus::NUMERICAL_ERROR);
      }
      if (crossingCount == 0) {
        firstCrossing = crossing;
      }
      lastCrossing = crossing;
      ++crossingCount;
    }
  }
  if (crossingCount < 2) {
    return Failure<double>(SvMathStatus::INSUFFICIENT_CROSSINGS);
  }

  const double periodSamples =
      (lastCrossing - firstCrossing) /
      static_cast<double>(crossingCount - 1);
  if (!IsFinite(periodSamples) || periodSamples <= 0.0) {
    return Failure<double>(SvMathStatus::NUMERICAL_ERROR);
  }
  const double frequency = sampleRateHz / periodSamples;
  if (!IsFinite(frequency) || frequency <= 0.0) {
    return Failure<double>(SvMathStatus::NUMERICAL_ERROR);
  }
  return Success(frequency);
}

SvMathResult<double> EstimateFrequencyByPhase(
    std::span<const double> phaseRadians, double sampleRateHz) noexcept {
  if (!ValidateSampleRate(sampleRateHz)) {
    return Failure<double>(SvMathStatus::INVALID_SAMPLE_RATE);
  }
  if (phaseRadians.size() < 2) {
    return Failure<double>(SvMathStatus::WINDOW_TOO_SHORT);
  }
  if (!AllFinite(phaseRadians)) {
    return Failure<double>(SvMathStatus::NON_FINITE_INPUT);
  }

  const long double sampleCount = static_cast<long double>(phaseRadians.size());
  const long double sumX = sampleCount * (sampleCount - 1.0L) / 2.0L;
  const long double sumXX =
      sampleCount * (sampleCount - 1.0L) * (2.0L * sampleCount - 1.0L) /
      6.0L;
  const long double denominator = sampleCount * sumXX - sumX * sumX;
  if (!std::isfinite(denominator) || denominator <= 0.0L) {
    return Failure<double>(SvMathStatus::NUMERICAL_ERROR);
  }

  long double unwrapped = phaseRadians.front();
  long double sumY = unwrapped;
  long double sumXY = 0.0L;
  for (std::size_t index = 1; index < phaseRadians.size(); ++index) {
    const long double previous = phaseRadians[index - 1];
    const long double current = phaseRadians[index];
    const long double delta = std::remainder(
        current - previous, 2.0L * std::numbers::pi_v<long double>);
    unwrapped += delta;
    const long double x = static_cast<long double>(index);
    sumY += unwrapped;
    sumXY += x * unwrapped;
  }
  // 第一项的 x=0，因此无需额外累加到 sumXY。
  const long double slope =
      (sampleCount * sumXY - sumX * sumY) / denominator;
  const long double frequency =
      slope * static_cast<long double>(sampleRateHz) /
      (2.0L * std::numbers::pi_v<long double>);
  const double result = static_cast<double>(frequency);
  if (!std::isfinite(frequency) || !IsFinite(result)) {
    return Failure<double>(SvMathStatus::NUMERICAL_ERROR);
  }
  if (std::abs(result) <= std::numeric_limits<double>::epsilon()) {
    return Failure<double>(SvMathStatus::INSUFFICIENT_PHASE_PROGRESS);
  }
  if (result < 0.0) {
    return Failure<double>(SvMathStatus::INVALID_FREQUENCY);
  }
  return Success(result);
}

SvMathResult<SvSinglePhasePower> ComputeSinglePhasePowerFromPhasors(
    const SvPhasor& voltage, const SvPhasor& current) noexcept {
  if (!IsFinite(voltage.real) || !IsFinite(voltage.imaginary) ||
      !IsFinite(current.real) || !IsFinite(current.imaginary)) {
    return Failure<SvSinglePhasePower>(SvMathStatus::NON_FINITE_INPUT);
  }

  const double active = voltage.real * current.real +
                        voltage.imaginary * current.imaginary;
  const double reactive = voltage.imaginary * current.real -
                          voltage.real * current.imaginary;
  const double apparent = std::hypot(active, reactive);
  const double powerFactor = apparent > 0.0 ? active / apparent : 0.0;
  if (!IsFinite(active) || !IsFinite(reactive) || !IsFinite(apparent) ||
      !IsFinite(powerFactor)) {
    return Failure<SvSinglePhasePower>(SvMathStatus::NUMERICAL_ERROR);
  }
  return Success(SvSinglePhasePower{.activeWatts = active,
                                    .reactiveVars = reactive,
                                    .apparentVa = apparent,
                                    .powerFactor = powerFactor});
}

SvMathResult<SvSinglePhasePower> ComputeSinglePhasePower(
    std::span<const double> voltageSamples,
    std::span<const double> currentSamples, double sampleRateHz,
    double frequencyHz) noexcept {
  if (voltageSamples.size() != currentSamples.size()) {
    return Failure<SvSinglePhasePower>(SvMathStatus::INPUT_SIZE_MISMATCH);
  }
  const auto voltage = ComputeSingleFrequencyPhasor(
      voltageSamples, sampleRateHz, frequencyHz);
  if (!voltage.ok()) {
    return Failure<SvSinglePhasePower>(voltage.status);
  }
  const auto current = ComputeSingleFrequencyPhasor(
      currentSamples, sampleRateHz, frequencyHz);
  if (!current.ok()) {
    return Failure<SvSinglePhasePower>(current.status);
  }
  return ComputeSinglePhasePowerFromPhasors(*voltage, *current);
}

}  // namespace IEC61850
