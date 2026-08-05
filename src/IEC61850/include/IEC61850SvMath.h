#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace IEC61850 {

// 数学函数只返回状态和可选结果，由调用方决定如何记录日志；避免热路径写日志。
enum class SvMathStatus : std::uint8_t {
  OK = 0,
  WINDOW_TOO_SHORT,
  NON_FINITE_INPUT,
  INVALID_INPUT,
  INVALID_SAMPLE_RATE,
  INVALID_FREQUENCY,
  INPUT_SIZE_MISMATCH,
  INSUFFICIENT_CROSSINGS,
  INSUFFICIENT_PHASE_PROGRESS,
  NUMERICAL_ERROR,
};

const char* ToString(SvMathStatus status) noexcept;

// 结果携带明确状态；失败时 value 为空，成功时 status 为 OK。
template <typename T>
struct SvMathResult {
  std::optional<T> value;
  SvMathStatus status = SvMathStatus::INVALID_INPUT;

  bool ok() const noexcept {
    return status == SvMathStatus::OK && value.has_value();
  }
  bool has_value() const noexcept { return ok(); }
  explicit operator bool() const noexcept { return ok(); }

  const T& operator*() const { return *value; }
  T& operator*() { return *value; }
  const T* operator->() const { return &*value; }
  T* operator->() { return &*value; }
};

struct SvPhasor {
  // 实部和虚部采用 RMS 量纲；相角定义为 atan2(imaginary, real)。
  double real = 0.0;
  double imaginary = 0.0;
  double magnitude = 0.0;
  double phaseRadians = 0.0;
};

struct SvSinglePhasePower {
  double activeWatts = 0.0;
  double reactiveVars = 0.0;
  double apparentVa = 0.0;
  double powerFactor = 0.0;
};

// 对有界采样窗口计算 RMS，不会复制或持有输入数据。
SvMathResult<double> ComputeRms(std::span<const double> samples) noexcept;

// 在给定采样率和目标频率上计算单频 RMS 相量（矩形窗 DFT）。
SvMathResult<SvPhasor> ComputeSingleFrequencyPhasor(
    std::span<const double> samples, double sampleRateHz,
    double frequencyHz) noexcept;

// 取相邻样本的线性插值上升过零点，并以首尾过零间隔估计频率。
SvMathResult<double> EstimateFrequencyByZeroCrossing(
    std::span<const double> samples, double sampleRateHz) noexcept;

// 对相位样本先逐点展开，再用线性回归估计相位斜率对应的频率。
SvMathResult<double> EstimateFrequencyByPhase(
    std::span<const double> phaseRadians, double sampleRateHz) noexcept;

// 由电压和电流基波 RMS 相量计算单相 P、Q、S 和功率因数。
SvMathResult<SvSinglePhasePower> ComputeSinglePhasePower(
    std::span<const double> voltageSamples,
    std::span<const double> currentSamples, double sampleRateHz,
    double frequencyHz) noexcept;

// 直接由两个 RMS 相量计算单相复功率，适合已有相量的调用方。
SvMathResult<SvSinglePhasePower> ComputeSinglePhasePowerFromPhasors(
    const SvPhasor& voltage, const SvPhasor& current) noexcept;

// 语义别名，便于协议层按“基波/过零/相位”命名调用。
inline SvMathResult<SvPhasor> EstimateFundamentalPhasor(
    std::span<const double> samples, double sampleRateHz,
    double frequencyHz) noexcept {
  return ComputeSingleFrequencyPhasor(samples, sampleRateHz, frequencyHz);
}

inline SvMathResult<double> EstimateFrequencyFromZeroCrossings(
    std::span<const double> samples, double sampleRateHz) noexcept {
  return EstimateFrequencyByZeroCrossing(samples, sampleRateHz);
}

inline SvMathResult<double> EstimateFrequencyFromPhase(
    std::span<const double> phaseRadians, double sampleRateHz) noexcept {
  return EstimateFrequencyByPhase(phaseRadians, sampleRateHz);
}

}  // namespace IEC61850
