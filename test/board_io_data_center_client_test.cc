#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "BoardIODataCenterClient.hpp"
#include "DataCenter_mock.grpc.pb.h"
#include "support/FakeDataCenter.hpp"

namespace BoardIO {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;

// 验证：已有 DigitalInput/board-di 连接会迁移到 BoardIO/board-di 并保留 conn_id。
TEST(BoardIODataCenterClientTest, MigratesLegacyInputConnectionAndKeepsId) {
  FakeDataCenterState state;
  DataCenterProto::GetOrCreateConnectionRequest legacyRequest;
  legacyRequest.mutable_key()->set_module_name("DigitalInput");
  legacyRequest.mutable_key()->set_conn_name("board-di");
  DataCenterProto::ConnectionInfo legacyConnection;
  ASSERT_TRUE(state.GetOrCreateConnection(legacyRequest, &legacyConnection).ok());

  auto stub = MakeStub(&state);
  BoardIODataCenterClient client;
  client.SetStub(stub);

  EXPECT_CALL(*stub, RenameConnection(_, _, _))
      .WillOnce(Invoke([&state](grpc::ClientContext*, const DataCenterProto::RenameConnectionRequest& request,
                               DataCenterProto::ConnectionInfo* response) {
        EXPECT_EQ(request.old_key().module_name(), "DigitalInput");
        EXPECT_EQ(request.old_key().conn_name(), "board-di");
        EXPECT_EQ(request.new_key().module_name(), "BoardIO");
        EXPECT_EQ(request.new_key().conn_name(), "board-di");
        return state.RenameConnection(request, response);
      }));

  DataCenterProto::ConnectionInfo response;
  ASSERT_TRUE(client.GetOrMigrateInputConnection(&response).ok());
  EXPECT_EQ(response.conn_id(), legacyConnection.conn_id());
  EXPECT_FALSE(state.HasConnection("DigitalInput", "board-di"));
  EXPECT_TRUE(state.HasConnection("BoardIO", "board-di"));
}

// 验证：旧 DI 连接不存在时回退创建 BoardIO/board-di 稳定端点。
TEST(BoardIODataCenterClientTest, CreatesInputConnectionWhenLegacyEndpointIsAbsent) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  BoardIODataCenterClient client;
  client.SetStub(stub);

  InSequence sequence;
  EXPECT_CALL(*stub, RenameConnection(_, _, _))
      .WillOnce(Invoke([&state](grpc::ClientContext*, const DataCenterProto::RenameConnectionRequest& request,
                               DataCenterProto::ConnectionInfo* response) {
        return state.RenameConnection(request, response);
      }));
  EXPECT_CALL(*stub, GetOrCreateConnection(_, _, _))
      .WillOnce(Invoke([&state](grpc::ClientContext*, const DataCenterProto::GetOrCreateConnectionRequest& request,
                               DataCenterProto::ConnectionInfo* response) {
        EXPECT_EQ(request.key().module_name(), "BoardIO");
        EXPECT_EQ(request.key().conn_name(), "board-di");
        return state.GetOrCreateConnection(request, response);
      }));

  DataCenterProto::ConnectionInfo response;
  ASSERT_TRUE(client.GetOrMigrateInputConnection(&response).ok());
  EXPECT_EQ(response.conn_id(), 1u);
  EXPECT_TRUE(state.HasConnection("BoardIO", "board-di"));
}

// 验证：输出连接创建请求使用 BoardIO/board-do 稳定端点。
TEST(BoardIODataCenterClientTest, CreatesStableOutputConnection) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  BoardIODataCenterClient client;
  client.SetStub(stub);

  EXPECT_CALL(*stub, GetOrCreateConnection(_, _, _))
      .WillOnce(Invoke([&state](grpc::ClientContext*, const DataCenterProto::GetOrCreateConnectionRequest& request,
                               DataCenterProto::ConnectionInfo* response) {
        EXPECT_EQ(request.key().module_name(), "BoardIO");
        EXPECT_EQ(request.key().conn_name(), "board-do");
        return state.GetOrCreateConnection(request, response);
      }));

  DataCenterProto::ConnectionInfo response;
  ASSERT_TRUE(client.GetOrCreateOutputConnection(&response).ok());
  EXPECT_EQ(response.conn_id(), 1u);
  EXPECT_TRUE(state.HasConnection("BoardIO", "board-do"));
}

// 验证：输入连接标签一次性覆盖为 DI1 至 DI4。
TEST(BoardIODataCenterClientTest, RegistersAllInputTags) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  BoardIODataCenterClient client;
  client.SetStub(stub);

  EXPECT_CALL(*stub, UpsertConnTags(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*, const DataCenterProto::UpsertConnTagsRequest& request,
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

  EXPECT_TRUE(client.RegisterInputTags(42).ok());
}

// 验证：输出连接标签一次性覆盖为 DO1 和 DO2。
TEST(BoardIODataCenterClientTest, RegistersAllOutputTags) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  BoardIODataCenterClient client;
  client.SetStub(stub);

  EXPECT_CALL(*stub, UpsertConnTags(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*, const DataCenterProto::UpsertConnTagsRequest& request,
                          DataCenterProto::Empty*) {
        EXPECT_EQ(request.conn_id(), 43u);
        EXPECT_TRUE(request.replace());
        EXPECT_EQ(request.tags_size(), 2);
        EXPECT_EQ(request.tags(0), "DO1");
        EXPECT_EQ(request.tags(1), "DO2");
        return grpc::Status::OK;
      }));

  EXPECT_TRUE(client.RegisterOutputTags(43).ok());
}

// 验证：DataCenter 已保存完整 DI 标签时不重复调用 UpsertConnTags。
TEST(BoardIODataCenterClientTest, KeepsCompleteInputTagsWithoutUpsert) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  BoardIODataCenterClient client;
  client.SetStub(stub);

  EXPECT_CALL(*stub, GetConnTags(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*, const DataCenterProto::GetConnTagsRequest& request,
                          DataCenterProto::ConnTags* response) {
        EXPECT_EQ(request.conn_id(), 42u);
        response->set_conn_id(42);
        response->add_tags("DI1");
        response->add_tags("DI2");
        response->add_tags("DI3");
        response->add_tags("DI4");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertConnTags(_, _, _)).Times(0);

  EXPECT_TRUE(client.EnsureInputTags(42).ok());
}

// 验证：DataCenter 缺失 DO 标签注册表时使用 replace=true 自动补齐 DO1 和 DO2。
TEST(BoardIODataCenterClientTest, RepairsMissingOutputTags) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  BoardIODataCenterClient client;
  client.SetStub(stub);

  InSequence sequence;
  EXPECT_CALL(*stub, GetConnTags(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*, const DataCenterProto::GetConnTagsRequest& request,
                          DataCenterProto::ConnTags*) {
        EXPECT_EQ(request.conn_id(), 43u);
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接标签不存在");
      }));
  EXPECT_CALL(*stub, UpsertConnTags(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*, const DataCenterProto::UpsertConnTagsRequest& request,
                          DataCenterProto::Empty*) {
        EXPECT_EQ(request.conn_id(), 43u);
        EXPECT_TRUE(request.replace());
        EXPECT_EQ(request.tags_size(), 2);
        EXPECT_EQ(request.tags(0), "DO1");
        EXPECT_EQ(request.tags(1), "DO2");
        return grpc::Status::OK;
      }));

  EXPECT_TRUE(client.EnsureOutputTags(43).ok());
}

// 验证：BOOL 发布请求携带输入连接、标签、质量和事件时间戳。
TEST(BoardIODataCenterClientTest, PublishesBoolEvent) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  BoardIODataCenterClient client;
  client.SetStub(stub);

  EXPECT_CALL(*stub, Publish(_, _, _))
      .WillOnce(Invoke([&state](grpc::ClientContext*, const DataCenterProto::PublishRequest& request,
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
}  // namespace BoardIO
