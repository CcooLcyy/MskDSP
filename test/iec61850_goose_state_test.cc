#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

#include "IEC61850GooseState.h"

namespace {

IEC61850::GooseMessage MakeMessage(std::uint32_t stateNumber,
                                    std::uint32_t sequenceNumber,
                                    std::int64_t receiveTimestampMs = 1000) {
  IEC61850::GooseMessage message;
  message.appId = 0x1001;
  message.gocbRef = "IED1LD0/LLN0$GO$gcb1";
  message.dataSetRef = "IED1LD0/LLN0$events";
  message.goId = "TripGOOSE";
  message.configRevision = 4;
  message.timeAllowedToLiveMs = 100;
  message.stateNumber = stateNumber;
  message.sequenceNumber = sequenceNumber;
  message.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  message.receiveTimestampMs = receiveTimestampMs;
  return message;
}

IEC61850::GooseSubscriptionConfig MakeConfig() {
  IEC61850::GooseSubscriptionConfig config;
  config.appIds.push_back(0x1001);
  config.gocbRef = "IED1LD0/LLN0$GO$gcb1";
  config.dataSetRef = "IED1LD0/LLN0$events";
  config.goId = "TripGOOSE";
  config.configRevision = 4;
  return config;
}

// 验证：有效GOOSE状态按stNum/sqNum接受，A/B相同状态只触发一次。
TEST(IEC61850GooseStateTest, AcceptsMonotonicStateAndDeduplicatesChannels) {
  IEC61850::GooseStateMachine state(MakeConfig());
  auto first = MakeMessage(1, 0);

  EXPECT_EQ(state.Process(first, 1000), IEC61850::GooseProcessResult::ACCEPTED);
  auto duplicate = first;
  duplicate.channel = IEC61850Proto::NETWORK_CHANNEL_B;
  EXPECT_EQ(state.Process(duplicate, 1001),
            IEC61850::GooseProcessResult::DUPLICATE);
  EXPECT_EQ(state.Process(MakeMessage(1, 1), 1002),
            IEC61850::GooseProcessResult::ACCEPTED);
  EXPECT_EQ(state.Process(MakeMessage(2, 0), 1003),
            IEC61850::GooseProcessResult::ACCEPTED);
  EXPECT_EQ(state.stateNumber(), 2u);
  EXPECT_EQ(state.sequenceNumber(), 0u);
}

// 验证：空订阅身份不会作为通配符接受任意GOOSE流。
TEST(IEC61850GooseStateTest, RejectsIncompleteSubscriptionIdentity) {
  IEC61850::GooseStateMachine state({});

  EXPECT_EQ(state.Process(MakeMessage(1, 0), 1000),
            IEC61850::GooseProcessResult::REJECTED);
}

// 验证：身份、ConfRev、ndsCom、仿真标志和非法序号都会被拒绝。
TEST(IEC61850GooseStateTest, RejectsInvalidIdentityAndSequence) {
  IEC61850::GooseStateMachine state(MakeConfig());
  auto message = MakeMessage(1, 0);
  message.gocbRef = "other";
  EXPECT_EQ(state.Process(message, 1000),
            IEC61850::GooseProcessResult::REJECTED);

  message = MakeMessage(1, 0);
  message.needsCommissioning = true;
  EXPECT_EQ(state.Process(message, 1000),
            IEC61850::GooseProcessResult::REJECTED);
  message = MakeMessage(1, 0);
  message.simulation = true;
  EXPECT_EQ(state.Process(message, 1000),
            IEC61850::GooseProcessResult::REJECTED);

  ASSERT_EQ(state.Process(MakeMessage(1, 0), 1000),
            IEC61850::GooseProcessResult::ACCEPTED);
  EXPECT_EQ(state.Process(MakeMessage(1, 0), 1001),
            IEC61850::GooseProcessResult::DUPLICATE);
  EXPECT_EQ(state.Process(MakeMessage(2, 1), 1002),
            IEC61850::GooseProcessResult::REJECTED);
  EXPECT_EQ(state.Process(MakeMessage(1, 2), 1003),
            IEC61850::GooseProcessResult::REJECTED);
}

// 验证：即使序号与最近报文相同，ndsCom、未授权仿真和零TTL仍必须按非法报文拒绝。
TEST(IEC61850GooseStateTest, RejectsInvalidFlagsBeforeDuplicateDetection) {
  IEC61850::GooseStateMachine state(MakeConfig());
  ASSERT_EQ(state.Process(MakeMessage(1, 0), 1000),
            IEC61850::GooseProcessResult::ACCEPTED);

  auto needsCommissioning = MakeMessage(1, 0);
  needsCommissioning.needsCommissioning = true;
  EXPECT_EQ(state.Process(needsCommissioning, 1001),
            IEC61850::GooseProcessResult::REJECTED);

  auto simulation = MakeMessage(1, 0);
  simulation.simulation = true;
  EXPECT_EQ(state.Process(simulation, 1002),
            IEC61850::GooseProcessResult::REJECTED);

  auto zeroTtl = MakeMessage(1, 0);
  zeroTtl.timeAllowedToLiveMs = 0;
  EXPECT_EQ(state.Process(zeroTtl, 1003),
            IEC61850::GooseProcessResult::REJECTED);
}

// 验证：DataSet成员数量、顺序、FC和值类型必须与订阅签名一致。
TEST(IEC61850GooseStateTest, ValidatesDataSetMemberSignature) {
  auto config = MakeConfig();
  config.members.push_back(
      {"IED1LD0/PTRC1.Tr.general", IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST,
       IEC61850::GooseValueType::BOOLEAN});
  IEC61850::GooseStateMachine state(std::move(config));

  auto valid = MakeMessage(1, 0);
  valid.values.push_back(
      {"IED1LD0/PTRC1.Tr.general",
       IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST, true});
  ASSERT_EQ(state.Process(valid, 1000),
            IEC61850::GooseProcessResult::ACCEPTED);

  auto wrongType = MakeMessage(1, 1);
  wrongType.values.push_back(
      {"IED1LD0/PTRC1.Tr.general",
       IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST, std::int64_t{1}});
  EXPECT_EQ(state.Process(wrongType, 1001),
            IEC61850::GooseProcessResult::REJECTED);

  auto missing = MakeMessage(1, 1);
  EXPECT_EQ(state.Process(missing, 1002),
            IEC61850::GooseProcessResult::REJECTED);
}

// 验证：A/B同序号载荷不一致会报告冲突，并保留上一份有效状态。
TEST(IEC61850GooseStateTest, DetectsConflictingDuplicatePayload) {
  auto config = MakeConfig();
  config.members.push_back(
      {"IED1LD0/PTRC1.Tr.general", IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST,
       IEC61850::GooseValueType::BOOLEAN});
  IEC61850::GooseStateMachine state(std::move(config));
  auto first = MakeMessage(1, 0);
  first.values.push_back(
      {"IED1LD0/PTRC1.Tr.general",
       IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST, true});
  ASSERT_EQ(state.Process(first, 1000),
            IEC61850::GooseProcessResult::ACCEPTED);

  auto conflicting = first;
  conflicting.channel = IEC61850Proto::NETWORK_CHANNEL_B;
  conflicting.values.front().value = false;
  EXPECT_EQ(state.Process(conflicting, 1001),
            IEC61850::GooseProcessResult::CONFLICT);
  ASSERT_EQ(state.values().size(), 1u);
  EXPECT_EQ(std::get<bool>(state.values().front().value), true);
}

// 验证：TTL到期只产生一次超时转换，新的有效状态可以恢复输入。
TEST(IEC61850GooseStateTest, TimesOutAndRecovers) {
  IEC61850::GooseStateMachine state(MakeConfig());
  ASSERT_EQ(state.Process(MakeMessage(1, 0), 1000),
            IEC61850::GooseProcessResult::ACCEPTED);
  EXPECT_EQ(state.CheckTimeout(1099), IEC61850::GooseProcessResult::NO_CHANGE);
  EXPECT_EQ(state.CheckTimeout(1100), IEC61850::GooseProcessResult::TIMED_OUT);
  EXPECT_EQ(state.state(), IEC61850::GooseInputState::TIMED_OUT);
  EXPECT_EQ(state.CheckTimeout(1200), IEC61850::GooseProcessResult::NO_CHANGE);
  EXPECT_EQ(state.Process(MakeMessage(2, 0), 1201),
            IEC61850::GooseProcessResult::ACCEPTED);
  EXPECT_EQ(state.state(), IEC61850::GooseInputState::ACTIVE);
}

// 验证重复GOOSE报文刷新TTL，超时后收到有效重发可以恢复活动状态。
TEST(IEC61850GooseStateTest, RefreshesTtlOnDuplicateAndRecoversAfterTimeout) {
  IEC61850::GooseStateMachine state(MakeConfig());
  ASSERT_EQ(state.Process(MakeMessage(1, 0), 1000),
            IEC61850::GooseProcessResult::ACCEPTED);
  EXPECT_EQ(state.Process(MakeMessage(1, 0), 1050),
            IEC61850::GooseProcessResult::DUPLICATE);
  EXPECT_EQ(state.CheckTimeout(1149), IEC61850::GooseProcessResult::NO_CHANGE);
  EXPECT_EQ(state.CheckTimeout(1150), IEC61850::GooseProcessResult::TIMED_OUT);

  EXPECT_EQ(state.Process(MakeMessage(1, 0), 1200),
            IEC61850::GooseProcessResult::RECOVERED);
  EXPECT_EQ(state.state(), IEC61850::GooseInputState::ACTIVE);
}

}  // namespace
