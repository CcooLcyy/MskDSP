#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "support/FakeDataCenter.hpp"

namespace {
using namespace std::chrono_literals;

DataCenterProto::SubscribeRequest SubscriptionRequest() {
  DataCenterProto::SubscribeRequest request;
  request.set_conn_id(42);
  request.add_tags("P_MEAS");
  return request;
}

DataCenterProto::PublishRequest Measurement() {
  DataCenterProto::PublishRequest request;
  request.set_conn_id(42);
  request.set_tag("P_MEAS");
  request.mutable_value()->set_double_value(30.0);
  request.set_quality(DataCenterProto::QUALITY_GOOD);
  return request;
}
}  // namespace

// 验证空闲超过旧 200 ms 截止时间仍保持订阅，随后发布的数据能够唤醒读取。
TEST(FakeDataCenterSubscriptionTest, IdleSubscriptionReceivesLaterMeasurement) {
  FakeDataCenterState state;
  auto reader = state.Subscribe(SubscriptionRequest());
  ASSERT_NE(reader, nullptr);
  std::promise<void> releasePublisher;
  auto release = releasePublisher.get_future();
  std::jthread publisher([&]() {
    release.wait();
    std::this_thread::sleep_for(350ms);
    EXPECT_TRUE(state.Publish(Measurement()).ok());
  });
  releasePublisher.set_value();
  DataCenterProto::PointUpdate update;
  EXPECT_TRUE(reader->Read(&update));
  EXPECT_EQ(update.value().double_value(), 30.0);
  EXPECT_EQ(state.GetSubscriptionCount(42), 1u);
  EXPECT_TRUE(reader->Finish().ok());
}

// 验证取消已绑定 ClientContext 会唤醒阻塞读取，并返回取消状态，而不是空闲超时成功。
TEST(FakeDataCenterSubscriptionTest, ContextCancellationWakesBlockedRead) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  grpc::ClientContext context;
  auto reader = stub->Subscribe(&context, SubscriptionRequest());
  ASSERT_NE(reader, nullptr);
  std::promise<void> entered;
  auto reading = entered.get_future();
  auto result = std::async(std::launch::async, [&]() {
    entered.set_value();
    DataCenterProto::PointUpdate update;
    return reader->Read(&update);
  });
  reading.wait();
  context.TryCancel();
  EXPECT_EQ(state.GetSubscriptionCount(42), 0u);
  const auto completed = result.wait_for(2s);
  EXPECT_EQ(completed, std::future_status::ready);
  if (completed != std::future_status::ready) {
    (void)reader->Finish();
  }
  EXPECT_FALSE(result.get());
  EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_EQ(state.GetSubscriptionCount(42), 0u);
}

// 验证订阅前取消仍有效，已有快照不会覆盖取消结果或被继续读取。
TEST(FakeDataCenterSubscriptionTest, PreCancelledContextRejectsQueuedSnapshot) {
  FakeDataCenterState state;
  EXPECT_TRUE(state.Publish(Measurement()).ok());
  auto stub = MakeStub(&state);
  grpc::ClientContext context;
  context.TryCancel();
  auto request = SubscriptionRequest();
  request.set_snapshot(true);
  auto reader = stub->Subscribe(&context, request);
  ASSERT_NE(reader, nullptr);
  DataCenterProto::PointUpdate update;
  EXPECT_FALSE(reader->Read(&update));
  EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_EQ(state.GetSubscriptionCount(42), 0u);
}

// 验证显式 Finish 唤醒阻塞读取，并维持正常结束状态。
TEST(FakeDataCenterSubscriptionTest, FinishWakesBlockedRead) {
  FakeDataCenterState state;
  auto reader = state.Subscribe(SubscriptionRequest());
  ASSERT_NE(reader, nullptr);
  auto result = std::async(std::launch::async, [&]() {
    DataCenterProto::PointUpdate update;
    return reader->Read(&update);
  });
  EXPECT_TRUE(reader->Finish().ok());
  EXPECT_EQ(result.wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(result.get());
  EXPECT_EQ(state.GetSubscriptionCount(42), 0u);
}

// 验证移除连接立即关闭对应订阅，不依赖空闲超时来回收读取线程。
TEST(FakeDataCenterSubscriptionTest, RemovingConnectionClosesSubscriptions) {
  for (const bool useDeleteRpc : {false, true}) {
    SCOPED_TRACE(useDeleteRpc ? "RPC 删除" : "直接移除");
    FakeDataCenterState state;
    state.AddConnection(42, "AGC", "group");
    auto reader = state.Subscribe(SubscriptionRequest());
    ASSERT_NE(reader, nullptr);
    if (useDeleteRpc) {
      DataCenterProto::DeleteConnectionRequest request;
      request.mutable_key()->set_module_name("AGC");
      request.mutable_key()->set_conn_name("group");
      EXPECT_TRUE(state.DeleteConnection(request).ok());
    } else {
      state.RemoveConnection("AGC", "group");
    }
    EXPECT_EQ(state.GetSubscriptionCount(42), 0u);
    DataCenterProto::PointUpdate update;
    EXPECT_FALSE(reader->Read(&update));
    EXPECT_TRUE(reader->Finish().ok());
  }
}

// 验证 Fake 被关闭时唤醒尚存读取器，关闭前排队的数据不得在关闭后继续交付。
TEST(FakeDataCenterSubscriptionTest, DestroyingStateClosesSurvivingReader) {
  auto state = std::make_unique<FakeDataCenterState>();
  EXPECT_TRUE(state->Publish(Measurement()).ok());
  auto request = SubscriptionRequest();
  request.set_snapshot(true);
  auto reader = state->Subscribe(request);
  ASSERT_NE(reader, nullptr);
  state.reset();
  DataCenterProto::PointUpdate update;
  EXPECT_FALSE(reader->Read(&update));
  EXPECT_TRUE(reader->Finish().ok());
}

// 验证请求标签倒序时，订阅快照仍按真实 DataCenter 的目标标签词典序交付。
TEST(FakeDataCenterSubscriptionTest, SnapshotOrdersTagsLikeRealDataCenter) {
  FakeDataCenterState state;
  auto first = Measurement();
  first.set_tag("Z_MEAS");
  auto second = Measurement();
  second.set_tag("A_MEAS");
  ASSERT_TRUE(state.Publish(first).ok());
  ASSERT_TRUE(state.Publish(second).ok());
  auto request = SubscriptionRequest();
  request.clear_tags();
  request.add_tags("Z_MEAS");
  request.add_tags("A_MEAS");
  request.set_snapshot(true);
  auto reader = state.Subscribe(request);
  ASSERT_NE(reader, nullptr);
  DataCenterProto::PointUpdate update;
  ASSERT_TRUE(reader->Read(&update));
  EXPECT_EQ(update.dst_tag(), "A_MEAS");
  ASSERT_TRUE(reader->Read(&update));
  EXPECT_EQ(update.dst_tag(), "Z_MEAS");
  EXPECT_TRUE(reader->Finish().ok());
}
