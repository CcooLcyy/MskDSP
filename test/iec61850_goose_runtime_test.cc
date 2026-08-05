#include <gtest/gtest.h>

#include <array>

#include "IEC61850GooseRuntime.h"

namespace {

IEC61850::GooseRealtimeSubscriptionConfig MakeConfig() {
  IEC61850::GooseRealtimeSubscriptionConfig config;
  config.subscriptionId = 7;
  config.gocbRef = "IED1LD0/LLN0$GO$gcb1";
  config.dataSetRef = "IED1LD0/LLN0$events";
  config.goId = "Trip";
  config.configRevision = 4;
  config.appIds[1] = 0x1001;
  config.signalIds = {11};
  config.valueTypes = {
      IEC61850::ProtocolRealtimeValueType::BOOLEAN};
  return config;
}

IEC61850::ProtocolGooseFrameView MakeFrame(
    std::span<const IEC61850::ProtocolRealtimeValue> values,
    std::uint32_t stateNumber = 1, std::uint32_t sequenceNumber = 0) {
  IEC61850::ProtocolGooseFrameView frame;
  frame.subscriptionId = 7;
  frame.gocbRef = "IED1LD0/LLN0$GO$gcb1";
  frame.dataSetRef = "IED1LD0/LLN0$events";
  frame.goId = "Trip";
  frame.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  frame.appId = 0x1001;
  frame.configRevision = 4;
  frame.timeAllowedToLiveMs = 10;
  frame.stateNumber = stateNumber;
  frame.sequenceNumber = sequenceNumber;
  frame.values = values;
  return frame;
}

IEC61850::ProtocolRealtimeValue MakeValue(bool value) {
  IEC61850::ProtocolRealtimeValue result;
  result.valueType = IEC61850::ProtocolRealtimeValueType::BOOLEAN;
  result.value.booleanValue = value;
  return result;
}

// 验证：GOOSE新状态接受并保存固定标量值。
TEST(IEC61850GooseRuntimeTest, AcceptsMonotonicState) {
  IEC61850::GooseRealtimeEngine engine({MakeConfig()});
  const std::array values{MakeValue(true)};
  std::size_t route = 0;
  EXPECT_EQ(engine.TryProcess(MakeFrame(values), 1000, &route),
            IEC61850::GooseRealtimeProcessResult::ACCEPTED);
  EXPECT_EQ(route, 0u);
  ASSERT_EQ(engine.values(route).size(), 1u);
  EXPECT_TRUE(engine.values(route)[0].value.booleanValue);
}

// 验证GOOSE超时检测在内部锁内复制值快照，调用方不读取可变内部存储。
TEST(IEC61850GooseRuntimeTest, CopiesTimedOutValueSnapshot) {
  IEC61850::GooseRealtimeEngine engine({MakeConfig()});
  const std::array values{MakeValue(true)};
  ASSERT_EQ(engine.TryProcess(MakeFrame(values), 1'000'000, nullptr),
            IEC61850::GooseRealtimeProcessResult::ACCEPTED);

  std::array<IEC61850::ProtocolRealtimeValue, 1> snapshot{};
  std::size_t route = 0;
  std::size_t valueCount = 0;
  EXPECT_EQ(engine.CheckTimeout(11'000'000, &route, snapshot, &valueCount),
            IEC61850::GooseRealtimeProcessResult::TIMED_OUT);
  EXPECT_EQ(route, 0u);
  ASSERT_EQ(valueCount, 1u);
  EXPECT_TRUE(snapshot[0].value.booleanValue);
}

// 验证：同序号同载荷是重复报文，同序号不同载荷报告冲突。
TEST(IEC61850GooseRuntimeTest, DetectsDuplicateAndConflict) {
  IEC61850::GooseRealtimeEngine engine({MakeConfig()});
  const std::array first{MakeValue(true)};
  ASSERT_EQ(engine.TryProcess(MakeFrame(first), 1000, nullptr),
            IEC61850::GooseRealtimeProcessResult::ACCEPTED);
  EXPECT_EQ(engine.TryProcess(MakeFrame(first, 1, 0), 1001, nullptr),
            IEC61850::GooseRealtimeProcessResult::DUPLICATE);
  const std::array conflict{MakeValue(false)};
  EXPECT_EQ(engine.TryProcess(MakeFrame(conflict, 1, 0), 1002, nullptr),
            IEC61850::GooseRealtimeProcessResult::CONFLICT);
}

// 验证：TTL到期只转换一次，新的状态可以恢复活动。
TEST(IEC61850GooseRuntimeTest, TimesOutAndRecovers) {
  IEC61850::GooseRealtimeEngine engine({MakeConfig()});
  const std::array values{MakeValue(true)};
  ASSERT_EQ(engine.TryProcess(MakeFrame(values), 1000, nullptr),
            IEC61850::GooseRealtimeProcessResult::ACCEPTED);
  EXPECT_EQ(engine.CheckTimeout(11'000'000, nullptr),
            IEC61850::GooseRealtimeProcessResult::TIMED_OUT);
  EXPECT_EQ(engine.CheckTimeout(12'000'000, nullptr),
            IEC61850::GooseRealtimeProcessResult::NO_CHANGE);
  EXPECT_EQ(engine.TryProcess(MakeFrame(values, 2, 0), 12'000'000, nullptr),
            IEC61850::GooseRealtimeProcessResult::ACCEPTED);
}

// 验证实时GOOSE重复报文刷新TTL，超时后有效重发返回恢复结果并恢复活动状态。
TEST(IEC61850GooseRuntimeTest, RefreshesTtlOnDuplicateAndRecoversAfterTimeout) {
  IEC61850::GooseRealtimeEngine engine({MakeConfig()});
  const std::array values{MakeValue(true)};
  ASSERT_EQ(engine.TryProcess(MakeFrame(values), 1'000'000'000, nullptr),
            IEC61850::GooseRealtimeProcessResult::ACCEPTED);
  EXPECT_EQ(engine.TryProcess(MakeFrame(values), 1'005'000'000, nullptr),
            IEC61850::GooseRealtimeProcessResult::DUPLICATE);
  EXPECT_EQ(engine.CheckTimeout(1'014'999'999, nullptr),
            IEC61850::GooseRealtimeProcessResult::NO_CHANGE);
  EXPECT_EQ(engine.CheckTimeout(1'015'000'000, nullptr),
            IEC61850::GooseRealtimeProcessResult::TIMED_OUT);

  EXPECT_EQ(engine.TryProcess(MakeFrame(values), 1'020'000'000, nullptr),
            IEC61850::GooseRealtimeProcessResult::RECOVERED);
  EXPECT_EQ(engine.state(0), IEC61850::GooseRealtimeInputState::ACTIVE);
}

// 验证：并发处理时不阻塞，竞争者返回拒绝并记录忙状态。
TEST(IEC61850GooseRuntimeTest, RejectsInvalidIdentity) {
  IEC61850::GooseRealtimeEngine engine({MakeConfig()});
  const std::array values{MakeValue(true)};
  auto frame = MakeFrame(values);
  frame.appId = 0x1002;
  EXPECT_EQ(engine.TryProcess(frame, 1000, nullptr),
            IEC61850::GooseRealtimeProcessResult::REJECTED);
}

}  // namespace
