#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <utility>

#include "IEC61850SvRuntime.h"

namespace {

IEC61850::SvRealtimeSubscriptionConfig MakeConfig() {
  IEC61850::SvRealtimeSubscriptionConfig config;
  config.streamId = 9;
  config.svId = "MU01";
  config.configRevision = 5;
  config.expectedAsduCount = 1;
  config.sampleWindowSize = 4;
  config.sampleRateHz = 4'000.0;
  config.nominalFrequencyHz = 50.0;
  config.appIds[1] = 0x1001;
  config.appIds[2] = 0x1002;
  config.valueTypes = {IEC61850::ProtocolRealtimeValueType::INTEGER};
  return config;
}

IEC61850::ProtocolRealtimeValue MakeValue(std::int64_t value) {
  IEC61850::ProtocolRealtimeValue result;
  result.valueType = IEC61850::ProtocolRealtimeValueType::INTEGER;
  result.value.integerValue = value;
  return result;
}

IEC61850::ProtocolRealtimeValue MakeBooleanValue(bool value) {
  IEC61850::ProtocolRealtimeValue result;
  result.valueType = IEC61850::ProtocolRealtimeValueType::BOOLEAN;
  result.value.booleanValue = value;
  return result;
}

IEC61850::ProtocolRealtimeValue MakeFloatingValue(double value) {
  IEC61850::ProtocolRealtimeValue result;
  result.valueType = IEC61850::ProtocolRealtimeValueType::FLOATING;
  result.value.floatingValue = value;
  return result;
}

IEC61850::ProtocolSvFrameView MakeFrame(
    std::span<const IEC61850::ProtocolRealtimeValue> values,
    std::uint16_t sampleCount = 1) {
  IEC61850::ProtocolSvFrameView frame;
  frame.streamId = 9;
  frame.svId = "MU01";
  frame.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  frame.appId = 0x1001;
  frame.configRevision = 5;
  frame.sampleCount = sampleCount;
  frame.asduCount = 1;
  frame.values = values;
  return frame;
}

// 验证SV额定频率解析保留50Hz兼容默认值，并支持60Hz工程配置。
TEST(IEC61850SvRuntimeTest, ResolvesSupportedNominalFrequency) {
  EXPECT_TRUE(IEC61850::IsSupportedSvNominalFrequencyHz(0.0));
  EXPECT_TRUE(IEC61850::IsSupportedSvNominalFrequencyHz(50.0));
  EXPECT_TRUE(IEC61850::IsSupportedSvNominalFrequencyHz(60.0));
  EXPECT_FALSE(IEC61850::IsSupportedSvNominalFrequencyHz(55.0));
  EXPECT_FALSE(IEC61850::IsSupportedSvNominalFrequencyHz(
      std::numeric_limits<double>::quiet_NaN()));
  EXPECT_DOUBLE_EQ(IEC61850::ResolveSvNominalFrequencyHz(0.0), 50.0);
  EXPECT_DOUBLE_EQ(IEC61850::ResolveSvNominalFrequencyHz(60.0), 60.0);
}

// 验证SV首帧接收后保留固定标量值。
TEST(IEC61850SvRuntimeTest, AcceptsInitialFrame) {
  IEC61850::SvRealtimeEngine engine({MakeConfig()});
  const std::array values{MakeValue(123)};
  std::size_t route = 0;

  EXPECT_EQ(engine.TryProcess(MakeFrame(values), &route),
            IEC61850::SvRealtimeProcessResult::ACCEPTED);
  EXPECT_EQ(route, 0u);
  ASSERT_EQ(engine.values(route).size(), 1u);
  EXPECT_EQ(engine.values(route)[0].value.integerValue, 123);
}

// 验证已接受的数值样本进入固定窗口，并按时间顺序复制给计算层。
TEST(IEC61850SvRuntimeTest, CopiesNumericSampleWindowInOrder) {
  auto config = MakeConfig();
  config.valueTypes = {IEC61850::ProtocolRealtimeValueType::FLOATING};
  IEC61850::SvRealtimeEngine engine({std::move(config)});
  for (std::uint16_t sampleCount = 1; sampleCount <= 3; ++sampleCount) {
    const auto value = MakeFloatingValue(static_cast<double>(sampleCount));
    ASSERT_EQ(engine.TryProcess(MakeFrame(std::array{value}, sampleCount),
                                nullptr),
              IEC61850::SvRealtimeProcessResult::ACCEPTED);
  }
  std::array<double, 4> output{};
  ASSERT_EQ(engine.CopyNumericSamples(0, 0, output), 3u);
  EXPECT_DOUBLE_EQ(output[0], 1.0);
  EXPECT_DOUBLE_EQ(output[1], 2.0);
  EXPECT_DOUBLE_EQ(output[2], 3.0);
}

// 验证采样计数出现缺口后清空旧窗口，避免跨缺口计算出伪造量。
TEST(IEC61850SvRuntimeTest, ResetsNumericWindowAfterSequenceGap) {
  auto config = MakeConfig();
  config.valueTypes = {IEC61850::ProtocolRealtimeValueType::FLOATING};
  IEC61850::SvRealtimeEngine engine({std::move(config)});
  const auto first = MakeFloatingValue(1.0);
  ASSERT_EQ(engine.TryProcess(MakeFrame(std::array{first}, 10), nullptr),
            IEC61850::SvRealtimeProcessResult::ACCEPTED);
  const auto gap = MakeFloatingValue(3.0);
  ASSERT_EQ(engine.TryProcess(MakeFrame(std::array{gap}, 12), nullptr),
            IEC61850::SvRealtimeProcessResult::SEQUENCE_GAP);
  std::array<double, 4> output{};
  ASSERT_EQ(engine.CopyNumericSamples(0, 0, output), 1u);
  EXPECT_DOUBLE_EQ(output[0], 3.0);
}

// 验证BOOL样本不进入数值窗口，避免把状态量交给模拟量算法。
TEST(IEC61850SvRuntimeTest, DoesNotCopyBooleanToNumericWindow) {
  auto config = MakeConfig();
  config.valueTypes = {IEC61850::ProtocolRealtimeValueType::BOOLEAN};
  IEC61850::SvRealtimeEngine engine({std::move(config)});
  const auto value = MakeBooleanValue(true);
  ASSERT_EQ(engine.TryProcess(MakeFrame(std::array{value}), nullptr),
            IEC61850::SvRealtimeProcessResult::ACCEPTED);
  std::array<double, 4> output{};
  EXPECT_EQ(engine.CopyNumericSamples(0, 0, output), 0u);
}

// 验证同一采样计数的同值报文去重、异值报文冲突。
TEST(IEC61850SvRuntimeTest, DetectsDuplicateAndConflict) {
  IEC61850::SvRealtimeEngine engine({MakeConfig()});
  const std::array first{MakeValue(123)};
  ASSERT_EQ(engine.TryProcess(MakeFrame(first), nullptr),
            IEC61850::SvRealtimeProcessResult::ACCEPTED);
  EXPECT_EQ(engine.TryProcess(MakeFrame(first), nullptr),
            IEC61850::SvRealtimeProcessResult::DUPLICATE);
  const std::array conflict{MakeValue(124)};
  EXPECT_EQ(engine.TryProcess(MakeFrame(conflict), nullptr),
            IEC61850::SvRealtimeProcessResult::CONFLICT);
}

// 验证采样计数缺口、回绕和乱序报文的处理结果。
TEST(IEC61850SvRuntimeTest, TracksGapAndRejectsOutOfOrder) {
  IEC61850::SvRealtimeEngine engine({MakeConfig()});
  const std::array values{MakeValue(123)};
  ASSERT_EQ(engine.TryProcess(MakeFrame(values, 0xffff), nullptr),
            IEC61850::SvRealtimeProcessResult::ACCEPTED);
  EXPECT_EQ(engine.TryProcess(MakeFrame(values, 1), nullptr),
            IEC61850::SvRealtimeProcessResult::SEQUENCE_GAP);
  EXPECT_EQ(engine.statistics().missingSamples, 1u);
  EXPECT_EQ(engine.TryProcess(MakeFrame(values, 0xff00), nullptr),
            IEC61850::SvRealtimeProcessResult::REJECTED);
}

// 验证SV身份、APPID和采样布局不匹配时整帧拒绝。
TEST(IEC61850SvRuntimeTest, RejectsIdentityAndLayoutMismatch) {
  IEC61850::SvRealtimeEngine engine({MakeConfig()});
  const std::array values{MakeValue(123)};
  auto frame = MakeFrame(values);
  frame.svId = "OTHER";
  EXPECT_EQ(engine.TryProcess(frame, nullptr),
            IEC61850::SvRealtimeProcessResult::REJECTED);
  frame = MakeFrame(values);
  frame.values = {};
  EXPECT_EQ(engine.TryProcess(frame, nullptr),
            IEC61850::SvRealtimeProcessResult::REJECTED);
}

// 验证同一SV报文中的多个ASDU按索引连续接收，下一采样从ASDU零开始。
TEST(IEC61850SvRuntimeTest, AcceptsMultipleAsdusInOrder) {
  auto config = MakeConfig();
  config.expectedAsduCount = 2;
  IEC61850::SvRealtimeEngine engine({std::move(config)});
  const std::array values{MakeValue(123)};
  auto first = MakeFrame(values, 10);
  first.asduCount = 2;
  first.asduIndex = 0;
  ASSERT_EQ(engine.TryProcess(first, nullptr),
            IEC61850::SvRealtimeProcessResult::ACCEPTED);
  auto second = first;
  second.asduIndex = 1;
  EXPECT_EQ(engine.TryProcess(second, nullptr),
            IEC61850::SvRealtimeProcessResult::ACCEPTED);
  auto next = first;
  next.sampleCount = 11;
  EXPECT_EQ(engine.TryProcess(next, nullptr),
            IEC61850::SvRealtimeProcessResult::ACCEPTED);
}

// 验证A网与B网APPID重复时SV实时订阅不会进入运行态。
TEST(IEC61850SvRuntimeTest, RejectsDuplicateAppIdIncludingAChannel) {
  auto config = MakeConfig();
  config.appIds[0] = 0x1001;
  config.appIds[1] = 0x1001;

  IEC61850::SvRealtimeEngine engine({std::move(config)});

  EXPECT_EQ(engine.size(), 0u);
}

// 验证同一SV报文的相邻ASDU不能携带不同采样计数。
TEST(IEC61850SvRuntimeTest, RejectsSampleCountChangeWithinAsduSequence) {
  auto config = MakeConfig();
  config.expectedAsduCount = 2;
  IEC61850::SvRealtimeEngine engine({std::move(config)});

  const std::array values{MakeValue(123)};
  auto first = MakeFrame(values, 10);
  first.asduCount = 2;
  first.asduIndex = 0;
  ASSERT_EQ(engine.TryProcess(first, nullptr),
            IEC61850::SvRealtimeProcessResult::ACCEPTED);

  auto second = first;
  second.asduIndex = 1;
  second.sampleCount = 11;

  EXPECT_EQ(engine.TryProcess(second, nullptr),
            IEC61850::SvRealtimeProcessResult::REJECTED);
}

// 验证双网完整多ASDU报文只接受一次，载荷冲突不会触发第二次更新。
TEST(IEC61850SvRuntimeTest, DeduplicatesCompletedFrameAcrossNetworks) {
  auto config = MakeConfig();
  config.expectedAsduCount = 2;
  IEC61850::SvRealtimeEngine engine({std::move(config)});
  const std::array values{MakeValue(123)};
  auto first = MakeFrame(values, 10);
  first.asduCount = 2;
  first.asduIndex = 0;
  ASSERT_EQ(engine.TryProcess(first, nullptr),
            IEC61850::SvRealtimeProcessResult::ACCEPTED);
  auto second = first;
  second.asduIndex = 1;
  ASSERT_EQ(engine.TryProcess(second, nullptr),
            IEC61850::SvRealtimeProcessResult::ACCEPTED);

  auto duplicate = first;
  duplicate.channel = IEC61850Proto::NETWORK_CHANNEL_B;
  duplicate.appId = 0x1002;
  EXPECT_EQ(engine.TryProcess(duplicate, nullptr),
            IEC61850::SvRealtimeProcessResult::DUPLICATE);
  duplicate.asduIndex = 1;
  EXPECT_EQ(engine.TryProcess(duplicate, nullptr),
            IEC61850::SvRealtimeProcessResult::DUPLICATE);

  auto conflict = first;
  conflict.channel = IEC61850Proto::NETWORK_CHANNEL_B;
  conflict.appId = 0x1002;
  conflict.values = std::array{MakeValue(999)};
  EXPECT_EQ(engine.TryProcess(conflict, nullptr),
            IEC61850::SvRealtimeProcessResult::CONFLICT);
}

}  // namespace
