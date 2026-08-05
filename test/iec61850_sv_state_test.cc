#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "IEC61850SvState.h"

namespace {

IEC61850::SvReceiveConfig MakeConfig(std::size_t bufferCapacity = 8) {
  IEC61850::SvReceiveConfig config;
  config.appId = 0x4001;
  config.svId = "MU01";
  config.configRevision = 5;
  config.expectedAsduCount = 1;
  config.channelCount = 2;
  config.expectedSmpSynch = 2;
  config.bufferCapacity = bufferCapacity;
  return config;
}

IEC61850::SvAsduView MakeAsdu(
    std::uint16_t sampleCount,
    std::span<const IEC61850::SvChannelSample> samples,
    std::string_view svId = "MU01", std::uint32_t configRevision = 5,
    std::uint8_t smpSynch = 2) {
  return IEC61850::SvAsduView{.svId = svId,
                              .smpCnt = sampleCount,
                              .configRevision = configRevision,
                              .smpSynch = smpSynch,
                              .samples = samples};
}

IEC61850::SvFrameView MakeFrame(
    std::span<const IEC61850::SvAsduView> asdus,
    std::uint16_t appId = 0x4001,
    IEC61850::SvNetworkChannel channel = IEC61850::SvNetworkChannel::A,
    std::int64_t receiveTimestampNs = 1'000'000) {
  return IEC61850::SvFrameView{.appId = appId,
                               .channel = channel,
                               .receiveTimestampNs = receiveTimestampNs,
                               .asdus = asdus};
}

constexpr std::array<IEC61850::SvChannelSample, 2> kSamples{{
    {.value = 101, .quality = 0x01},
    {.value = -202, .quality = 0x02},
}};

// 验证：固定容量缓冲按布局保存样本，并在容量满时覆盖最旧样本。
TEST(IEC61850SvSampleRingBufferTest, PreservesLayoutAndOverwritesOldest) {
  IEC61850::SvSampleRingBuffer buffer(2, kSamples.size());
  ASSERT_TRUE(buffer.valid());
  IEC61850::SvSampleMetadata metadata;
  metadata.appId = 0x4001;
  metadata.channel = IEC61850::SvNetworkChannel::A;
  metadata.smpCnt = 10;

  EXPECT_EQ(buffer.Push(metadata, kSamples),
            IEC61850::SvBufferPushResult::ENQUEUED);
  metadata.smpCnt = 11;
  EXPECT_EQ(buffer.Push(metadata, kSamples),
            IEC61850::SvBufferPushResult::ENQUEUED);
  metadata.smpCnt = 12;
  EXPECT_EQ(buffer.Push(metadata, kSamples),
            IEC61850::SvBufferPushResult::OVERWROTE_OLDEST);
  EXPECT_EQ(buffer.size(), 2u);

  std::array<IEC61850::SvChannelSample, 2> output{};
  IEC61850::SvSampleMetadata outputMetadata;
  ASSERT_TRUE(buffer.TryPop(&outputMetadata, output));
  EXPECT_EQ(outputMetadata.smpCnt, 11u);
  EXPECT_EQ(output, kSamples);
  ASSERT_TRUE(buffer.TryPop(&outputMetadata, output));
  EXPECT_EQ(outputMetadata.smpCnt, 12u);
  EXPECT_TRUE(buffer.empty());
}

// 验证：输出空间不足时缓冲不弹出样本，错误布局也不会写入缓冲。
TEST(IEC61850SvSampleRingBufferTest, RejectsLayoutMismatchAtomically) {
  IEC61850::SvSampleRingBuffer buffer(2, kSamples.size());
  IEC61850::SvSampleMetadata metadata;
  const std::array<IEC61850::SvChannelSample, 1> shortSamples{{
      {.value = 1, .quality = 0},
  }};

  EXPECT_EQ(buffer.Push(metadata, shortSamples),
            IEC61850::SvBufferPushResult::REJECTED_LAYOUT);
  EXPECT_TRUE(buffer.empty());
  ASSERT_EQ(buffer.Push(metadata, kSamples),
            IEC61850::SvBufferPushResult::ENQUEUED);
  std::array<IEC61850::SvChannelSample, 1> shortOutput{};
  EXPECT_FALSE(buffer.TryPop(&metadata, shortOutput));
  EXPECT_EQ(buffer.size(), 1u);
}

// 验证：有效SV帧按ASDU布局进入缓冲并完整保留来源元数据。
TEST(IEC61850SvReceiveStateTest, AcceptsExpectedFrameAndPreservesMetadata) {
  IEC61850::SvReceiveState state(MakeConfig());
  ASSERT_TRUE(state.configValid());
  const std::array asdus{MakeAsdu(100, kSamples)};

  const auto result = state.Process(MakeFrame(
      asdus, 0x4001, IEC61850::SvNetworkChannel::B, 9'000'000));

  EXPECT_TRUE(result.accepted);
  EXPECT_FALSE(result.sequenceGap);
  EXPECT_FALSE(result.bufferOverflow);
  EXPECT_EQ(result.rejectReason, IEC61850::SvRejectReason::NONE);
  std::array<IEC61850::SvChannelSample, 2> output{};
  IEC61850::SvSampleMetadata metadata;
  ASSERT_TRUE(state.TryPop(&metadata, output));
  EXPECT_EQ(metadata.appId, 0x4001u);
  EXPECT_EQ(metadata.channel, IEC61850::SvNetworkChannel::B);
  EXPECT_EQ(metadata.smpCnt, 100u);
  EXPECT_EQ(metadata.smpSynch, 2u);
  EXPECT_EQ(metadata.receiveTimestampNs, 9'000'000);
  EXPECT_EQ(output, kSamples);
}

// 验证：ASDU数量或任一成员布局不符时整帧拒绝且不会推进采样序号。
TEST(IEC61850SvReceiveStateTest, RejectsAsduCountAndLayoutAtomically) {
  auto config = MakeConfig();
  config.expectedAsduCount = 2;
  IEC61850::SvReceiveState state(config);
  const std::array oneAsdu{MakeAsdu(20, kSamples)};
  auto result = state.Process(MakeFrame(oneAsdu));
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.rejectReason,
            IEC61850::SvRejectReason::ASDU_COUNT_MISMATCH);

  const std::array<IEC61850::SvChannelSample, 1> shortSamples{{
      {.value = 7, .quality = 0},
  }};
  const std::array invalidAsdus{MakeAsdu(20, kSamples),
                                MakeAsdu(21, shortSamples)};
  result = state.Process(MakeFrame(invalidAsdus));
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.rejectReason,
            IEC61850::SvRejectReason::SAMPLE_LAYOUT_MISMATCH);
  EXPECT_EQ(state.bufferedSamples(), 0u);

  const std::array validAsdus{MakeAsdu(1, kSamples),
                              MakeAsdu(2, kSamples)};
  EXPECT_TRUE(state.Process(MakeFrame(validAsdus)).accepted);
  EXPECT_EQ(state.lastSmpCnt(), 2u);
}

// 验证：APPID、svID、confRev和smpSynch任一不符都会被分别拒绝。
TEST(IEC61850SvReceiveStateTest, RejectsIdentityRevisionAndSynchronization) {
  IEC61850::SvReceiveState state(MakeConfig());
  const std::array validAsdus{MakeAsdu(1, kSamples)};
  auto result = state.Process(MakeFrame(validAsdus, 0x4002));
  EXPECT_EQ(result.rejectReason, IEC61850::SvRejectReason::APP_ID_MISMATCH);

  const std::array wrongId{MakeAsdu(1, kSamples, "OTHER")};
  result = state.Process(MakeFrame(wrongId));
  EXPECT_EQ(result.rejectReason, IEC61850::SvRejectReason::SV_ID_MISMATCH);

  const std::array wrongRevision{MakeAsdu(1, kSamples, "MU01", 6)};
  result = state.Process(MakeFrame(wrongRevision));
  EXPECT_EQ(result.rejectReason,
            IEC61850::SvRejectReason::CONFIG_REVISION_MISMATCH);

  const std::array invalidSynch{MakeAsdu(1, kSamples, "MU01", 5, 3)};
  result = state.Process(MakeFrame(invalidSynch));
  EXPECT_EQ(result.rejectReason,
            IEC61850::SvRejectReason::SMP_SYNCH_INVALID);

  const std::array wrongSynch{MakeAsdu(1, kSamples, "MU01", 5, 1)};
  result = state.Process(MakeFrame(wrongSynch));
  EXPECT_EQ(result.rejectReason,
            IEC61850::SvRejectReason::SMP_SYNCH_MISMATCH);
  EXPECT_EQ(state.statistics().framesInvalid, 5u);
  EXPECT_EQ(state.bufferedSamples(), 0u);
}

// 验证：smpCnt按16位无符号计数连续推进并接受65535到0的标准回绕。
TEST(IEC61850SvReceiveStateTest, AcceptsContinuousCountAndSixteenBitWrap) {
  IEC61850::SvReceiveState state(MakeConfig());
  const std::array first{MakeAsdu(65534, kSamples)};
  const std::array second{MakeAsdu(65535, kSamples)};
  const std::array wrapped{MakeAsdu(0, kSamples)};

  EXPECT_TRUE(state.Process(MakeFrame(first)).accepted);
  EXPECT_TRUE(state.Process(MakeFrame(second)).accepted);
  const auto result = state.Process(MakeFrame(wrapped));

  EXPECT_TRUE(result.accepted);
  EXPECT_FALSE(result.sequenceGap);
  EXPECT_EQ(state.lastSmpCnt(), 0u);
  EXPECT_EQ(state.statistics().samplesDropped, 0u);
}

// 验证：前向采样缺口仍接收当前帧，并准确累计缺失样本数。
TEST(IEC61850SvReceiveStateTest, AcceptsForwardGapAndCountsMissingSamples) {
  IEC61850::SvReceiveState state(MakeConfig());
  const std::array first{MakeAsdu(10, kSamples)};
  const std::array afterGap{MakeAsdu(13, kSamples)};
  ASSERT_TRUE(state.Process(MakeFrame(first)).accepted);

  const auto result = state.Process(MakeFrame(afterGap));

  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.sequenceGap);
  EXPECT_EQ(result.missingSamples, 2u);
  EXPECT_EQ(state.statistics().sequenceGapEvents, 1u);
  EXPECT_EQ(state.statistics().samplesDropped, 2u);
  EXPECT_EQ(state.lastSmpCnt(), 13u);
}

// 验证：重复或旧序样本拒绝且不改变最后序号，后续连续样本仍可接收。
TEST(IEC61850SvReceiveStateTest, RejectsDuplicateAndOutOfOrderSamples) {
  IEC61850::SvReceiveState state(MakeConfig());
  const std::array first{MakeAsdu(10, kSamples)};
  ASSERT_TRUE(state.Process(MakeFrame(first)).accepted);

  auto result = state.Process(MakeFrame(first));
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.rejectReason,
            IEC61850::SvRejectReason::DUPLICATE_SAMPLE);
  const std::array older{MakeAsdu(9, kSamples)};
  result = state.Process(MakeFrame(older));
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.rejectReason,
            IEC61850::SvRejectReason::OUT_OF_ORDER_SAMPLE);
  EXPECT_EQ(state.lastSmpCnt(), 10u);

  const std::array next{MakeAsdu(11, kSamples)};
  EXPECT_TRUE(state.Process(MakeFrame(next)).accepted);
  EXPECT_EQ(state.statistics().duplicateFrames, 1u);
  EXPECT_EQ(state.statistics().outOfOrderFrames, 1u);
}

// 验证：会话重置清空序号和缓冲，但保留本次模块生命周期累计统计。
TEST(IEC61850SvReceiveStateTest, ResetSessionAllowsNewSequenceAndKeepsStatistics) {
  IEC61850::SvReceiveState state(MakeConfig());
  const std::array first{MakeAsdu(500, kSamples)};
  ASSERT_TRUE(state.Process(MakeFrame(first)).accepted);
  ASSERT_EQ(state.bufferedSamples(), 1u);

  state.ResetSession();

  EXPECT_EQ(state.bufferedSamples(), 0u);
  EXPECT_FALSE(state.hasLastSmpCnt());
  EXPECT_EQ(state.statistics().sessionResets, 1u);
  const std::array restarted{MakeAsdu(1, kSamples)};
  EXPECT_TRUE(state.Process(MakeFrame(restarted)).accepted);
  EXPECT_EQ(state.lastSmpCnt(), 1u);
  EXPECT_EQ(state.statistics().framesReceived, 2u);
}

// 验证：接收缓冲满时保留最新样本、累计覆盖丢样并维持固定最高水位。
TEST(IEC61850SvReceiveStateTest, OverwritesOldestAndCountsBufferOverflow) {
  IEC61850::SvReceiveState state(MakeConfig(2));
  for (std::uint16_t count = 1; count <= 2; ++count) {
    const std::array asdus{MakeAsdu(count, kSamples)};
    ASSERT_TRUE(state.Process(MakeFrame(asdus)).accepted);
  }
  const std::array third{MakeAsdu(3, kSamples)};

  const auto result = state.Process(MakeFrame(third));

  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.bufferOverflow);
  EXPECT_EQ(result.overwrittenSamples, 1u);
  EXPECT_EQ(state.statistics().bufferOverflowEvents, 1u);
  EXPECT_EQ(state.statistics().samplesOverwritten, 1u);
  EXPECT_EQ(state.statistics().samplesDropped, 1u);
  EXPECT_EQ(state.statistics().bufferHighWatermark, 2u);
  std::array<IEC61850::SvChannelSample, 2> output{};
  IEC61850::SvSampleMetadata metadata;
  ASSERT_TRUE(state.TryPop(&metadata, output));
  EXPECT_EQ(metadata.smpCnt, 2u);
  ASSERT_TRUE(state.TryPop(&metadata, output));
  EXPECT_EQ(metadata.smpCnt, 3u);
}

// 验证：无效容量、ASDU数量或采样布局配置在处理任何帧前即被拒绝。
TEST(IEC61850SvReceiveStateTest, RejectsInvalidConfiguration) {
  auto config = MakeConfig();
  config.expectedAsduCount = 2;
  config.bufferCapacity = 1;
  IEC61850::SvReceiveState state(config);
  EXPECT_FALSE(state.configValid());
  const std::array asdus{MakeAsdu(1, kSamples)};

  const auto result = state.Process(MakeFrame(asdus));

  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.rejectReason,
            IEC61850::SvRejectReason::INVALID_CONFIGURATION);
  EXPECT_EQ(state.bufferCapacity(), 0u);
}

}  // namespace
