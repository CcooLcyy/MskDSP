#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "IEC61850SvMathEngine.h"

namespace {

IEC61850::SvMathStreamPlan MakePlan(std::uint32_t expectedAsduCount = 1) {
  IEC61850::SvMathStreamPlan plan;
  plan.streamId = 3;
  plan.samplesPerCycle = 4;
  plan.expectedAsduCount = expectedAsduCount;
  plan.nominalFrequencyHz = 50.0;
  plan.members.push_back({.inputSignalId = 10, .rmsSignalId = 20});
  return plan;
}

IEC61850::RealtimeSignalUpdate MakeUpdate(double value,
                                           std::uint16_t sampleCount) {
  IEC61850::RealtimeSignalUpdate update;
  update.signalId = 10;
  update.sessionGeneration = 7;
  update.source = IEC61850::RealtimeSignalSource::SV_DERIVED;
  update.valueType = IEC61850::RealtimeSignalValueType::FLOATING;
  update.qualityBits = 0;
  update.timestampNs = static_cast<std::int64_t>(sampleCount) * 1'000'000;
  update.sequence = static_cast<std::uint64_t>(sampleCount) << 32;
  update.value.floatingValue = value;
  return update;
}

}  // namespace

// 验证单周期样本收齐后生成稳定信号ID的RMS派生量。
TEST(IEC61850SvMathEngineTest, EmitsRmsAfterCompleteWindow) {
  IEC61850::SvMathEngine engine({MakePlan()}, 7);
  std::array<IEC61850::RealtimeSignalUpdate, 1> outputs{};

  EXPECT_EQ(engine.maxOutputCount(), 1u);
  EXPECT_EQ(engine.Process(MakeUpdate(1.0, 1), 1, outputs), 0u);
  EXPECT_EQ(engine.Process(MakeUpdate(2.0, 2), 2, outputs), 0u);
  EXPECT_EQ(engine.Process(MakeUpdate(3.0, 3), 3, outputs), 0u);
  ASSERT_EQ(engine.Process(MakeUpdate(4.0, 4), 4, outputs), 1u);
  EXPECT_EQ(outputs[0].signalId, 20u);
  EXPECT_EQ(outputs[0].sessionGeneration, 7u);
  EXPECT_EQ(outputs[0].valueType,
            IEC61850::RealtimeSignalValueType::FLOATING);
  EXPECT_DOUBLE_EQ(outputs[0].value.floatingValue, std::sqrt(7.5));
}

// 验证SV数学引擎拒绝多ASDU计划，避免把ASDU序列误当成独立采样点。
TEST(IEC61850SvMathEngineTest, RejectsMultipleAsduPlan) {
  IEC61850::SvMathEngine engine({MakePlan(2)}, 7);
  EXPECT_EQ(engine.streamCount(), 0u);
  EXPECT_EQ(engine.maxOutputCount(), 0u);
}

// 验证品质异常会清空窗口，恢复有效数据后必须重新收齐完整窗口。
TEST(IEC61850SvMathEngineTest, ResetsWindowOnInvalidQuality) {
  IEC61850::SvMathEngine engine({MakePlan()}, 7);
  std::array<IEC61850::RealtimeSignalUpdate, 1> outputs{};
  for (std::uint16_t sample = 1; sample <= 3; ++sample) {
    ASSERT_EQ(engine.Process(MakeUpdate(1.0, sample), sample, outputs), 0u);
  }
  auto invalid = MakeUpdate(1.0, 4);
  invalid.qualityBits = 1;
  EXPECT_EQ(engine.Process(invalid, 4, outputs), 0u);
  EXPECT_EQ(engine.Process(MakeUpdate(1.0, 5), 5, outputs), 0u);
  EXPECT_EQ(engine.Process(MakeUpdate(1.0, 6), 6, outputs), 0u);
  EXPECT_EQ(engine.Process(MakeUpdate(1.0, 7), 7, outputs), 0u);
  EXPECT_EQ(engine.Process(MakeUpdate(1.0, 8), 8, outputs), 1u);
}

// 验证采样计数缺口和非有限输入不会产生伪造RMS输出。
TEST(IEC61850SvMathEngineTest, RejectsGapAndNonFiniteInput) {
  IEC61850::SvMathEngine engine({MakePlan()}, 7);
  std::array<IEC61850::RealtimeSignalUpdate, 1> outputs{};
  EXPECT_EQ(engine.Process(MakeUpdate(1.0, 1), 1, outputs), 0u);
  EXPECT_EQ(engine.Process(MakeUpdate(1.0, 3), 3, outputs), 0u);
  auto nonFinite = MakeUpdate(std::numeric_limits<double>::quiet_NaN(), 4);
  EXPECT_EQ(engine.Process(nonFinite, 4, outputs), 0u);
}
