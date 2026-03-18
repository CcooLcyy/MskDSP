#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "DataCenter_mock.grpc.pb.h"
#include "ModbusRTUDataCenterClient.h"
#include "support/FakeDataCenter.hpp"

namespace {
using ModbusRTU::DataCenterClient;

using ::testing::_;
using ::testing::Invoke;
}  // 命名空间结束

// 验证：ConnectionExists 能匹配到目标模块和连接。
TEST(ModbusRtuDataCenterClientTest, ConnectionExistsFindsMatch) {
  FakeDataCenterState state;
  state.AddConnection(7, "ModbusRTU", "conn-1");
  auto stub = MakeStub(&state);

  DataCenterClient client("ModbusRTU");
  client.setStub(stub);

  bool exists = false;
  ASSERT_TRUE(client.ConnectionExists("conn-1", &exists).ok());
  EXPECT_TRUE(exists);
}

// 验证：ConnectionExists 会拒绝空的 conn_name。
TEST(ModbusRtuDataCenterClientTest, ConnectionExistsRejectsEmptyName) {
  DataCenterClient client("ModbusRTU");
  bool exists = false;
  auto st = client.ConnectionExists("", &exists);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：GetLatest 会正确转发请求并返回更新结果。
TEST(ModbusRtuDataCenterClientTest, GetLatestReturnsUpdates) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  DataCenterClient client("ModbusRTU");
  client.setStub(stub);

  EXPECT_CALL(*stub, GetLatest(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const DataCenterProto::GetLatestRequest& req,
                          DataCenterProto::GetLatestResponse* resp) {
        EXPECT_EQ(req.conn_id(), 12u);
        EXPECT_EQ(req.tags_size(), 1);
        if (req.tags_size() > 0) {
          EXPECT_EQ(req.tags(0), "tag-1");
        }
        auto* update = resp->add_updates();
        update->set_dst_tag("tag-1");
        update->mutable_value()->set_bool_value(true);
        return grpc::Status::OK;
      }));

  DataCenterProto::GetLatestResponse resp;
  auto st = client.GetLatest(12, {"tag-1"}, &resp);
  ASSERT_TRUE(st.ok());
  ASSERT_EQ(resp.updates_size(), 1);
  EXPECT_EQ(resp.updates(0).dst_tag(), "tag-1");
  EXPECT_TRUE(resp.updates(0).value().bool_value());
}

// 验证：PublishBool 会设置 bool_value 并携带元信息。
TEST(ModbusRtuDataCenterClientTest, PublishBoolBuildsRequest) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  DataCenterClient client("ModbusRTU");
  client.setStub(stub);

  EXPECT_CALL(*stub, Publish(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const DataCenterProto::PublishRequest& req,
                          DataCenterProto::Empty*) {
        EXPECT_EQ(req.conn_id(), 9u);
        EXPECT_EQ(req.tag(), "coil-1");
        EXPECT_EQ(req.value().kind_case(), DataCenterProto::PointValue::kBoolValue);
        EXPECT_TRUE(req.value().bool_value());
        EXPECT_EQ(req.quality(), DataCenterProto::QUALITY_GOOD);
        EXPECT_EQ(req.ts_ms(), 123);
        return grpc::Status::OK;
      }));

  auto st = client.PublishBool(9, "coil-1", true, DataCenterProto::QUALITY_GOOD, 123);
  EXPECT_TRUE(st.ok());
}

// 验证：PublishUInt16 会使用 int_value 表示无符号寄存器值。
TEST(ModbusRtuDataCenterClientTest, PublishUInt16BuildsRequest) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  DataCenterClient client("ModbusRTU");
  client.setStub(stub);

  EXPECT_CALL(*stub, Publish(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const DataCenterProto::PublishRequest& req,
                          DataCenterProto::Empty*) {
        EXPECT_EQ(req.conn_id(), 10u);
        EXPECT_EQ(req.tag(), "reg-1");
        EXPECT_EQ(req.value().kind_case(), DataCenterProto::PointValue::kIntValue);
        EXPECT_EQ(req.value().int_value(), 65535);
        EXPECT_EQ(req.quality(), DataCenterProto::QUALITY_UNCERTAIN);
        EXPECT_EQ(req.ts_ms(), 456);
        return grpc::Status::OK;
      }));

  auto st = client.PublishUInt16(10, "reg-1", 65535, DataCenterProto::QUALITY_UNCERTAIN, 456);
  EXPECT_TRUE(st.ok());
}

// 验证：PublishDouble 使用 double_value 并携带元信息。
TEST(ModbusRtuDataCenterClientTest, PublishDoubleBuildsRequest) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  DataCenterClient client("ModbusRTU");
  client.setStub(stub);

  EXPECT_CALL(*stub, Publish(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const DataCenterProto::PublishRequest& req,
                          DataCenterProto::Empty*) {
        EXPECT_EQ(req.conn_id(), 11u);
        EXPECT_EQ(req.tag(), "reg-2");
        EXPECT_EQ(req.value().kind_case(), DataCenterProto::PointValue::kDoubleValue);
        EXPECT_DOUBLE_EQ(req.value().double_value(), 220.5);
        EXPECT_EQ(req.quality(), DataCenterProto::QUALITY_GOOD);
        EXPECT_EQ(req.ts_ms(), 789);
        return grpc::Status::OK;
      }));

  auto st = client.PublishDouble(11, "reg-2", 220.5, DataCenterProto::QUALITY_GOOD, 789);
  EXPECT_TRUE(st.ok());
}
