#include <gtest/gtest.h>

#include <array>

#include "IEC61850RealtimeSignalProcessor.h"

namespace {

IEC61850::ProtocolSignalDefinition MakeDefinition(
    std::uint32_t signalId, IEC61850Proto::PointValueType valueType) {
  IEC61850::ProtocolSignalDefinition definition;
  definition.signalId = signalId;
  definition.valueType = valueType;
  return definition;
}

IEC61850::RealtimeSignalUpdate MakeUpdate(
    std::uint32_t signalId, std::uint64_t generation,
    IEC61850::RealtimeSignalValueType valueType) {
  IEC61850::RealtimeSignalUpdate update;
  update.signalId = signalId;
  update.sessionGeneration = generation;
  update.source = IEC61850::RealtimeSignalSource::GOOSE;
  update.valueType = valueType;
  update.channel = IEC61850::RealtimeNetworkChannel::A;
  update.timestampNs = 1234;
  update.sequence = 9;
  update.value.booleanValue = true;
  return update;
}

// 验证实时更新能够写入固定快照并保留来源、序号和标量值。
TEST(IEC61850RealtimeSignalProcessorTest, PublishesAndLoadsSnapshot) {
  const std::array definitions{
      MakeDefinition(2, IEC61850Proto::POINT_VALUE_TYPE_BOOL),
      MakeDefinition(1, IEC61850Proto::POINT_VALUE_TYPE_BOOL)};
  IEC61850::RealtimeSignalProcessor processor(definitions, 7);
  auto update = MakeUpdate(1, 7,
                           IEC61850::RealtimeSignalValueType::BOOLEAN);

  EXPECT_EQ(processor.Process(update),
            IEC61850::RealtimeSignalProcessResult::ACCEPTED);
  IEC61850::RealtimeSignalSnapshot snapshot;
  ASSERT_TRUE(processor.Load(1, &snapshot));
  EXPECT_TRUE(snapshot.valid);
  EXPECT_EQ(snapshot.signalId, 1u);
  EXPECT_EQ(snapshot.sessionGeneration, 7u);
  EXPECT_EQ(snapshot.sequence, 9u);
  EXPECT_TRUE(snapshot.value.booleanValue);
}

// 验证旧会话、未知点和类型不匹配更新不会污染实时快照。
TEST(IEC61850RealtimeSignalProcessorTest, RejectsInvalidUpdates) {
  const std::array definitions{
      MakeDefinition(1, IEC61850Proto::POINT_VALUE_TYPE_BOOL)};
  IEC61850::RealtimeSignalProcessor processor(definitions, 7);
  auto update = MakeUpdate(1, 6,
                           IEC61850::RealtimeSignalValueType::BOOLEAN);
  EXPECT_EQ(processor.Process(update),
            IEC61850::RealtimeSignalProcessResult::SESSION_MISMATCH);

  update = MakeUpdate(9, 7, IEC61850::RealtimeSignalValueType::BOOLEAN);
  EXPECT_EQ(processor.Process(update),
            IEC61850::RealtimeSignalProcessResult::UNKNOWN_SIGNAL);

  update = MakeUpdate(1, 7, IEC61850::RealtimeSignalValueType::INTEGER);
  EXPECT_EQ(processor.Process(update),
            IEC61850::RealtimeSignalProcessResult::TYPE_MISMATCH);
  IEC61850::RealtimeSignalSnapshot snapshot;
  EXPECT_FALSE(processor.Load(1, &snapshot));
}

}  // namespace
