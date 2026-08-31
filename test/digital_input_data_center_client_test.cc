#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DataCenter_mock.grpc.pb.h"
#include "DigitalInputDataCenterClient.hpp"
#include "support/FakeDataCenter.hpp"

namespace DigitalInput {
namespace {

using ::testing::_;
using ::testing::Invoke;

// 验证：注册连接请求使用 DigitalInput/board-di 稳定端点。
TEST(DigitalInputDataCenterClientTest, GetOrCreateUsesStableBoardEndpoint) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  DigitalInputDataCenterClient client;
  client.SetStub(stub);

  EXPECT_CALL(*stub, GetOrCreateConnection(_, _, _))
      .WillOnce(Invoke([&state](grpc::ClientContext*,
                          const DataCenterProto::GetOrCreateConnectionRequest& request,
                          DataCenterProto::ConnectionInfo* response) {
        EXPECT_EQ(request.key().module_name(), "DigitalInput");
        EXPECT_EQ(request.key().conn_name(), "board-di");
        return state.GetOrCreateConnection(request, response);
      }));

  DataCenterProto::ConnectionInfo response;
  ASSERT_TRUE(client.GetOrCreateBoardConnection(&response).ok());
  EXPECT_EQ(response.conn_id(), 1u);
  EXPECT_TRUE(state.HasConnection("DigitalInput", "board-di"));
}

// 验证：连接标签注册一次性覆盖为 DI1 至 DI4。
TEST(DigitalInputDataCenterClientTest, RegistersAllDigitalInputTags) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  DigitalInputDataCenterClient client;
  client.SetStub(stub);

  EXPECT_CALL(*stub, UpsertConnTags(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const DataCenterProto::UpsertConnTagsRequest& request,
                          DataCenterProto::Empty*) {
        EXPECT_EQ(request.conn_id(), 42u);
        EXPECT_TRUE(request.replace());
        EXPECT_EQ(request.tags_size(), 4);
        EXPECT_EQ(request.tags(0), "DI1");
        EXPECT_EQ(request.tags(1), "DI2");
        EXPECT_EQ(request.tags(2), "DI3");
        EXPECT_EQ(request.tags(3), "DI4");
        return grpc::Status::OK;
      }));

  EXPECT_TRUE(client.RegisterBoardTags(42).ok());
}

// 验证：BOOL 发布请求携带连接、标签、质量和事件时间戳。
TEST(DigitalInputDataCenterClientTest, PublishesBoolEvent) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  DigitalInputDataCenterClient client;
  client.SetStub(stub);

  EXPECT_CALL(*stub, Publish(_, _, _))
      .WillOnce(Invoke([&state](grpc::ClientContext*,
                          const DataCenterProto::PublishRequest& request,
                          DataCenterProto::Empty*) {
        EXPECT_EQ(request.conn_id(), 42u);
        EXPECT_EQ(request.tag(), "DI2");
        EXPECT_EQ(request.value().kind_case(), DataCenterProto::PointValue::kBoolValue);
        EXPECT_TRUE(request.value().bool_value());
        EXPECT_EQ(request.quality(), DataCenterProto::QUALITY_GOOD);
        EXPECT_EQ(request.ts_ms(), 1234);
        return state.Publish(request);
      }));

  EXPECT_TRUE(client.PublishBool(42, "DI2", true, 1234).ok());
}

}  // namespace
}  // namespace DigitalInput
