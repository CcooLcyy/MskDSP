#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

#include "IEC61850RealtimeSignalBus.h"

namespace {

IEC61850::RealtimeSignalUpdate MakeUpdate(std::uint64_t generation,
                                           std::uint32_t signalId) {
  IEC61850::RealtimeSignalUpdate update;
  update.signalId = signalId;
  update.sessionGeneration = generation;
  update.source = IEC61850::RealtimeSignalSource::GOOSE;
  update.valueType = IEC61850::RealtimeSignalValueType::BOOLEAN;
  update.channel = IEC61850::RealtimeNetworkChannel::A;
  update.timestampNs = 1234;
  update.sequence = 7;
  update.value.booleanValue = true;
  return update;
}

// 验证：固定大小的实时值可通过单生产者队列完整传递。
TEST(IEC61850RealtimeSignalBusTest, PublishesAndConsumesFixedUpdate) {
  IEC61850::RealtimeSignalBus bus(1, 2, 11);
  auto producer = bus.producer(0);
  auto input = MakeUpdate(11, 42);

  ASSERT_TRUE(producer.TryPublish(input));
  IEC61850::RealtimeSignalUpdate output;
  ASSERT_TRUE(bus.TryConsume(&output));
  EXPECT_EQ(output.signalId, 42u);
  EXPECT_EQ(output.sessionGeneration, 11u);
  EXPECT_EQ(output.value.booleanValue, true);
  EXPECT_EQ(bus.statistics().published, 1u);
  EXPECT_EQ(bus.statistics().consumed, 1u);
}

// 验证：队列满时非阻塞丢弃新值并锁存溢出诊断，不覆盖已排队值。
TEST(IEC61850RealtimeSignalBusTest, DropsNewValueWhenQueueIsFull) {
  IEC61850::RealtimeSignalBus bus(1, 1, 11);
  auto producer = bus.producer(0);
  ASSERT_TRUE(producer.TryPublish(MakeUpdate(11, 1)));
  EXPECT_FALSE(producer.TryPublish(MakeUpdate(11, 2)));

  const auto statistics = bus.statistics();
  EXPECT_EQ(statistics.dropped, 1u);
  EXPECT_TRUE(statistics.overflowLatched);
  IEC61850::RealtimeSignalUpdate output;
  ASSERT_TRUE(bus.TryConsume(&output));
  EXPECT_EQ(output.signalId, 1u);
}

// 验证：旧会话代际和总线失效后，迟到生产者不能再进入实时路径。
TEST(IEC61850RealtimeSignalBusTest, RejectsStaleGenerationAndInvalidatedBus) {
  IEC61850::RealtimeSignalBus bus(1, 2, 11);
  auto producer = bus.producer(0);
  auto stale = MakeUpdate(10, 1);
  EXPECT_FALSE(producer.TryPublish(stale));
  EXPECT_TRUE(producer.TryPublish(MakeUpdate(11, 2)));

  bus.Invalidate();
  EXPECT_FALSE(bus.IsActive());
  EXPECT_FALSE(producer.TryPublish(MakeUpdate(11, 3)));
  IEC61850::RealtimeSignalUpdate output;
  EXPECT_FALSE(bus.TryConsume(&output));
  EXPECT_EQ(bus.statistics().rejectedInactive, 2u);
}

// 验证：多个协议生产者各自使用独立SPSC队列，单一消费者可以合并读取。
TEST(IEC61850RealtimeSignalBusTest, MergesIndependentProducerQueues) {
  IEC61850::RealtimeSignalBus bus(2, 2, 11);
  auto first = bus.producer(0);
  auto second = bus.producer(1);
  ASSERT_TRUE(first.TryPublish(MakeUpdate(11, 10)));
  ASSERT_TRUE(second.TryPublish(MakeUpdate(11, 20)));

  IEC61850::RealtimeSignalUpdate output;
  ASSERT_TRUE(bus.TryConsume(&output));
  EXPECT_EQ(output.signalId, 10u);
  ASSERT_TRUE(bus.TryConsume(&output));
  EXPECT_EQ(output.signalId, 20u);
  EXPECT_FALSE(bus.TryConsume(&output));
}

// 验证：零生产者、零容量和零会话代际被拒绝，避免产生不可用实时总线。
TEST(IEC61850RealtimeSignalBusTest, RejectsInvalidConstruction) {
  EXPECT_THROW(IEC61850::RealtimeSignalBus(0, 1, 1), std::invalid_argument);
  EXPECT_THROW(IEC61850::RealtimeSignalBus(1, 0, 1), std::invalid_argument);
  EXPECT_THROW(IEC61850::RealtimeSignalBus(1, 1, 0), std::invalid_argument);
}

}  // namespace
