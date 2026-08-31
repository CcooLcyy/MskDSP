#include <gtest/gtest.h>

#include <vector>

#include "DigitalInputEventProcessor.hpp"

namespace DigitalInput {
namespace {

// 验证：四个 GPIO offset 映射到稳定的 DI 标签。
TEST(DigitalInputEventProcessorTest, MapsGpioOffsetsToTags) {
  const auto di1 = TagForOffset(114);
  const auto di2 = TagForOffset(116);
  const auto di3 = TagForOffset(113);
  const auto di4 = TagForOffset(115);
  ASSERT_TRUE(di1.has_value());
  ASSERT_TRUE(di2.has_value());
  ASSERT_TRUE(di3.has_value());
  ASSERT_TRUE(di4.has_value());
  EXPECT_EQ(*di1, "DI1");
  EXPECT_EQ(*di2, "DI2");
  EXPECT_EQ(*di3, "DI3");
  EXPECT_EQ(*di4, "DI4");
  EXPECT_FALSE(TagForOffset(112).has_value());
}

// 验证：物理低电平（短接）转换为业务有效值 true。
TEST(DigitalInputEventProcessorTest, ConvertsActiveLowPhysicalLevel) {
  EXPECT_TRUE(PhysicalLevelToLogical(false));
  EXPECT_FALSE(PhysicalLevelToLogical(true));
}

// 验证：严格 SOE 不发布启动初值，但收到首个边沿后发布该点状态。
TEST(DigitalInputEventProcessorTest, PublishesFirstEdgeWithoutInitialSnapshot) {
  std::vector<PublishedDigitalInput> published;
  DigitalInputEventProcessor processor([&published](const PublishedDigitalInput& event) {
    published.push_back(event);
    return true;
  });

  EXPECT_TRUE(published.empty());
  EXPECT_TRUE(processor.HandleEvent(GpioEvent{.offset = 114, .physicalHigh = false, .timestampMs = 1234}));
  ASSERT_EQ(published.size(), 1u);
  EXPECT_EQ(published[0].tag, "DI1");
  EXPECT_TRUE(published[0].value);
  EXPECT_EQ(published[0].timestampMs, 1234);
}

// 验证：同一电平重复事件不会产生重复 SOE，实际变位仍会发布。
TEST(DigitalInputEventProcessorTest, PublishesOnlyWhenValueChanges) {
  std::vector<PublishedDigitalInput> published;
  DigitalInputEventProcessor processor([&published](const PublishedDigitalInput& event) {
    published.push_back(event);
    return true;
  });

  EXPECT_TRUE(processor.HandleEvent(GpioEvent{.offset = 113, .physicalHigh = true, .timestampMs = 10}));
  EXPECT_FALSE(processor.HandleEvent(GpioEvent{.offset = 113, .physicalHigh = true, .timestampMs = 11}));
  EXPECT_TRUE(processor.HandleEvent(GpioEvent{.offset = 113, .physicalHigh = false, .timestampMs = 12}));

  ASSERT_EQ(published.size(), 2u);
  EXPECT_FALSE(published[0].value);
  EXPECT_TRUE(published[1].value);
  EXPECT_EQ(published[1].timestampMs, 12);
}

// 验证：未知 GPIO offset 被忽略，不会错误发布到其他 DI 标签。
TEST(DigitalInputEventProcessorTest, IgnoresUnknownOffset) {
  std::vector<PublishedDigitalInput> published;
  DigitalInputEventProcessor processor([&published](const PublishedDigitalInput& event) {
    published.push_back(event);
    return true;
  });

  EXPECT_FALSE(processor.HandleEvent(GpioEvent{.offset = 999, .physicalHigh = false, .timestampMs = 1}));
  EXPECT_TRUE(published.empty());
}

// 验证：DataCenter 发布失败时不确认状态，后续同值事件仍可重试发布。
TEST(DigitalInputEventProcessorTest, RetriesAfterPublishFailure) {
  int attempts = 0;
  DigitalInputEventProcessor processor([&attempts](const PublishedDigitalInput&) {
    ++attempts;
    return attempts > 1;
  });

  EXPECT_FALSE(processor.HandleEvent(GpioEvent{.offset = 114, .physicalHigh = false, .timestampMs = 20}));
  EXPECT_TRUE(processor.HandleEvent(GpioEvent{.offset = 114, .physicalHigh = false, .timestampMs = 21}));
  EXPECT_EQ(attempts, 2);
}

}  // namespace
}  // namespace DigitalInput
