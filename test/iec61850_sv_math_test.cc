#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

#include "IEC61850SvMath.h"

namespace {

using IEC61850::SvMathStatus;

constexpr double kSampleRateHz = 1'000.0;
constexpr double kFrequencyHz = 50.0;

// 生成相位固定的正弦采样，便于验证基波相量和功率公式。
template <std::size_t N>
std::array<double, N> MakeCosine(double amplitude, double phaseRadians,
                                 double frequencyHz = kFrequencyHz) {
  std::array<double, N> samples{};
  for (std::size_t index = 0; index < N; ++index) {
    const double time = static_cast<double>(index) / kSampleRateHz;
    samples[index] = amplitude *
                     std::cos(2.0 * std::numbers::pi * frequencyHz * time +
                              phaseRadians);
  }
  return samples;
}

// 验证：有限非空窗口的均方根按所有样本计算。
TEST(IEC61850SvMathTest, ComputesRms) {
  constexpr std::array samples{3.0, 4.0};

  const auto result = IEC61850::ComputeRms(samples);

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result.value.has_value());
  EXPECT_DOUBLE_EQ(*result.value, std::sqrt(12.5));
}

// 验证：空窗口和非有限样本返回明确状态，不产生数值结果。
TEST(IEC61850SvMathTest, RejectsEmptyAndNonFiniteRmsInput) {
  const std::array<double, 0> empty{};
  const auto emptyResult = IEC61850::ComputeRms(empty);
  EXPECT_FALSE(emptyResult.ok());
  EXPECT_EQ(emptyResult.status, SvMathStatus::WINDOW_TOO_SHORT);

  const std::array nonFinite{1.0, std::numeric_limits<double>::quiet_NaN()};
  const auto nonFiniteResult = IEC61850::ComputeRms(nonFinite);
  EXPECT_FALSE(nonFiniteResult.ok());
  EXPECT_EQ(nonFiniteResult.status, SvMathStatus::NON_FINITE_INPUT);
}

// 验证：整周期窗口的单频 DFT 相量输出 RMS 幅值、实部、虚部和相角。
TEST(IEC61850SvMathTest, ComputesFundamentalPhasor) {
  constexpr std::size_t kSamples = 40;
  constexpr double kPeak = 2.0;
  constexpr double kPhase = 0.3;
  const auto input = MakeCosine<kSamples>(kPeak, kPhase);

  const auto result = IEC61850::ComputeSingleFrequencyPhasor(
      input, kSampleRateHz, kFrequencyHz);

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result.value.has_value());
  const auto& phasor = *result.value;
  EXPECT_NEAR(phasor.magnitude, kPeak / std::sqrt(2.0), 1e-12);
  EXPECT_NEAR(phasor.real, phasor.magnitude * std::cos(kPhase), 1e-12);
  EXPECT_NEAR(phasor.imaginary, phasor.magnitude * std::sin(kPhase), 1e-12);
  EXPECT_NEAR(phasor.phaseRadians, kPhase, 1e-12);
}

// 验证：采样率、频率或窗口长度非法时，相量计算返回对应状态。
TEST(IEC61850SvMathTest, RejectsInvalidPhasorParameters) {
  constexpr std::array samples{1.0, 0.0, -1.0, 0.0};

  EXPECT_EQ(IEC61850::ComputeSingleFrequencyPhasor(samples, 0.0, 50.0)
                .status,
            SvMathStatus::INVALID_SAMPLE_RATE);
  EXPECT_EQ(IEC61850::ComputeSingleFrequencyPhasor(samples, 1'000.0, 0.0)
                .status,
            SvMathStatus::INVALID_FREQUENCY);
  EXPECT_EQ(IEC61850::ComputeSingleFrequencyPhasor(samples, 1'000.0, 50.0)
                .status,
            SvMathStatus::WINDOW_TOO_SHORT);
}

// 验证：跨越多个周期的上升过零点可以得到采样频率对应的真实频率。
TEST(IEC61850SvMathTest, EstimatesFrequencyByZeroCrossing) {
  const auto input = MakeCosine<200>(1.0, -std::numbers::pi / 2.0);

  const auto result = IEC61850::EstimateFrequencyByZeroCrossing(
      input, kSampleRateHz);

  ASSERT_TRUE(result.ok());
  EXPECT_NEAR(*result.value, kFrequencyHz, 1e-10);
}

// 验证：没有足够上升过零点时不臆造频率结果。
TEST(IEC61850SvMathTest, RejectsInsufficientZeroCrossings) {
  constexpr std::array input{0.0, 0.2, 0.3, 0.1, 0.0};

  const auto result = IEC61850::EstimateFrequencyByZeroCrossing(
      input, kSampleRateHz);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status, SvMathStatus::INSUFFICIENT_CROSSINGS);
}

// 验证：相位展开后的线性相位序列可以估计正频率。
TEST(IEC61850SvMathTest, EstimatesFrequencyByPhase) {
  constexpr std::size_t kSamples = 120;
  std::array<double, kSamples> phases{};
  for (std::size_t index = 0; index < phases.size(); ++index) {
    phases[index] = 0.4 + 2.0 * std::numbers::pi * kFrequencyHz *
                              static_cast<double>(index) / kSampleRateHz;
  }

  const auto result = IEC61850::EstimateFrequencyByPhase(phases,
                                                          kSampleRateHz);

  ASSERT_TRUE(result.ok());
  EXPECT_NEAR(*result.value, kFrequencyHz, 1e-10);
}

// 验证：相位窗口不足或无相位推进时返回明确状态。
TEST(IEC61850SvMathTest, RejectsInvalidPhaseWindow) {
  const std::array onePhase{0.1};
  EXPECT_EQ(IEC61850::EstimateFrequencyByPhase(onePhase, kSampleRateHz)
                .status,
            SvMathStatus::WINDOW_TOO_SHORT);

  const std::array constantPhase{0.1, 0.1, 0.1};
  EXPECT_EQ(IEC61850::EstimateFrequencyByPhase(constantPhase, kSampleRateHz)
                .status,
            SvMathStatus::INSUFFICIENT_PHASE_PROGRESS);
}

// 验证：电压、电流基波相量按单相复功率公式计算有功和无功。
TEST(IEC61850SvMathTest, ComputesSinglePhasePower) {
  constexpr std::size_t kSamples = 40;
  constexpr double kVoltageRms = 230.0;
  constexpr double kCurrentRms = 10.0;
  constexpr double kCurrentPhase = -0.4;
  const auto voltage = MakeCosine<kSamples>(kVoltageRms * std::sqrt(2.0), 0.0);
  const auto current = MakeCosine<kSamples>(kCurrentRms * std::sqrt(2.0),
                                            kCurrentPhase);

  const auto result = IEC61850::ComputeSinglePhasePower(
      voltage, current, kSampleRateHz, kFrequencyHz);

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result.value.has_value());
  const auto& power = *result.value;
  EXPECT_NEAR(power.activeWatts, kVoltageRms * kCurrentRms * std::cos(0.4),
              1e-9);
  EXPECT_NEAR(power.reactiveVars, kVoltageRms * kCurrentRms * std::sin(0.4),
              1e-9);
  EXPECT_NEAR(power.apparentVa, kVoltageRms * kCurrentRms, 1e-9);
  EXPECT_NEAR(power.powerFactor, std::cos(0.4), 1e-12);
}

// 验证：电压、电流窗口长度不一致时，功率计算拒绝输入。
TEST(IEC61850SvMathTest, RejectsMismatchedPowerWindows) {
  constexpr std::array voltage{1.0, 2.0, 3.0, 4.0};
  constexpr std::array current{1.0, 2.0, 3.0};

  const auto result = IEC61850::ComputeSinglePhasePower(
      voltage, current, kSampleRateHz, kFrequencyHz);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status, SvMathStatus::INPUT_SIZE_MISMATCH);
}

}  // namespace
