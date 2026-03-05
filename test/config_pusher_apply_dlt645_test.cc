#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ConfigPusherApplyDlt645.h"
#include "DLT645_mock.grpc.pb.h"

namespace {
using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;

ConfigPusherProto::Dlt645Config MakeDlt645Config(bool start) {
  ConfigPusherProto::Dlt645Config config;
  auto *mqtt = config.mutable_mqtt();
  mqtt->set_host("127.0.0.1");
  mqtt->set_port(1883);
  mqtt->set_client_id("dlt645-test");

  auto *task = config.add_links();
  auto *linkCfg = task->mutable_link()->mutable_config();
  linkCfg->set_conn_name("conv_{device_no}");
  linkCfg->set_protocol_variant(DLT645Proto::PROTOCOL_VARIANT_DLT645_PCD);
  linkCfg->set_meter_addr("202601200001");
  linkCfg->set_transport_type(DLT645Proto::TRANSPORT_MQTT_UART);
  linkCfg->set_comm_mode(DLT645Proto::COMM_MODE_LORA);
  task->add_device_nos("01");
  task->add_device_nos("0A");

  auto *point = task->mutable_point_table()->add_points();
  point->set_tag("P");
  point->set_di("02010100");
  point->set_data_len(2);
  point->set_type(DLT645Proto::DATA_TYPE_UINT16);
  point->set_access(DLT645Proto::ACCESS_READ_ONLY);

  task->set_start(start);
  return config;
}
}  // namespace

// 验证：DLT645 gRPC stub 为空时下发失败。
TEST(ConfigPusherApplyDlt645Test, NullStubReturnsFalse) {
  ConfigPusherProto::Dlt645Config config;
  EXPECT_FALSE(ConfigPusher::applyDlt645Config(config, nullptr));
}

// 验证：启用 device_nos 后会展开多条 UpsertLink/UpsertPointTable 请求并替换连接名。
TEST(ConfigPusherApplyDlt645Test, ExpandDeviceNosForLinkAndPointTable) {
  auto config = MakeDlt645Config(false);
  auto stub = std::make_unique<DLT645Proto::MockDLT645ServiceStub>();

  InSequence seq;
  EXPECT_CALL(*stub, UpdateConfig(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const DLT645Proto::UpdateConfigRequest &req,
                          DLT645Proto::UpdateConfigResponse *resp) {
        EXPECT_EQ(req.mqtt().client_id(), "dlt645-test");
        resp->set_ok(true);
        resp->set_message("成功");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertLink(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const DLT645Proto::UpsertLinkRequest &req,
                          DLT645Proto::LinkInfo *resp) {
        EXPECT_EQ(req.config().conn_name(), "conv_01");
        EXPECT_EQ(req.config().device_no(), "01");
        resp->set_conn_id(101);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertPointTable(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const DLT645Proto::UpsertPointTableRequest &req,
                          DLT645Proto::Empty *) {
        EXPECT_EQ(req.conn_name(), "conv_01");
        EXPECT_EQ(req.points_size(), 1);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertLink(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const DLT645Proto::UpsertLinkRequest &req,
                          DLT645Proto::LinkInfo *resp) {
        EXPECT_EQ(req.config().conn_name(), "conv_0A");
        EXPECT_EQ(req.config().device_no(), "0A");
        resp->set_conn_id(102);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertPointTable(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const DLT645Proto::UpsertPointTableRequest &req,
                          DLT645Proto::Empty *) {
        EXPECT_EQ(req.conn_name(), "conv_0A");
        EXPECT_EQ(req.points_size(), 1);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartLink(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyDlt645Config(config, stub.get()));
}

// 验证：device_nos 中存在非法值时，不会继续下发连接配置并返回失败。
TEST(ConfigPusherApplyDlt645Test, InvalidDeviceNoStopsApply) {
  auto config = MakeDlt645Config(false);
  auto *task = config.mutable_links(0);
  task->clear_device_nos();
  task->add_device_nos("GG");

  auto stub = std::make_unique<DLT645Proto::MockDLT645ServiceStub>();
  EXPECT_CALL(*stub, UpdateConfig(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const DLT645Proto::UpdateConfigRequest &,
                          DLT645Proto::UpdateConfigResponse *resp) {
        resp->set_ok(true);
        resp->set_message("成功");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertLink(_, _, _)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyDlt645Config(config, stub.get()));
}
