#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_set>

#include "ConfigPusherApplyDlt645.h"
#include "DLT645_mock.grpc.pb.h"

namespace {
using ::testing::_;
using ::testing::AtLeast;
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
  linkCfg->set_poll_item_interval_ms(200);
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
  EXPECT_CALL(*stub, ListLinks(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const DLT645Proto::Empty &,
                          DLT645Proto::ListLinksResponse *resp) {
        resp->Clear();
        return grpc::Status::OK;
      }));
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
        EXPECT_EQ(req.config().poll_item_interval_ms(), 200u);
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
        EXPECT_EQ(req.config().poll_item_interval_ms(), 200u);
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
  EXPECT_CALL(*stub, ListLinks(_, _, _)).Times(0);
  EXPECT_CALL(*stub, UpdateConfig(_, _, _)).Times(0);
  EXPECT_CALL(*stub, UpsertLink(_, _, _)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyDlt645Config(config, stub.get()));
}

// 验证：point_table.conn_name 含 {device_no} 占位符时，会按每个设备序号分别展开。
TEST(ConfigPusherApplyDlt645Test, ExpandsPointTableConnNamePlaceholderPerDeviceNo) {
  auto config = MakeDlt645Config(false);
  config.mutable_links(0)->mutable_point_table()->set_conn_name("pt_{device_no}");
  auto stub = std::make_unique<DLT645Proto::MockDLT645ServiceStub>();

  InSequence seq;
  EXPECT_CALL(*stub, ListLinks(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const DLT645Proto::Empty &,
                          DLT645Proto::ListLinksResponse *resp) {
        resp->Clear();
        return grpc::Status::OK;
      }));
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
        EXPECT_EQ(req.config().poll_item_interval_ms(), 200u);
        resp->set_conn_id(201);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertPointTable(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const DLT645Proto::UpsertPointTableRequest &req,
                          DLT645Proto::Empty *) {
        EXPECT_EQ(req.conn_name(), "pt_01");
        EXPECT_EQ(req.points_size(), 1);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertLink(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const DLT645Proto::UpsertLinkRequest &req,
                          DLT645Proto::LinkInfo *resp) {
        EXPECT_EQ(req.config().conn_name(), "conv_0A");
        EXPECT_EQ(req.config().poll_item_interval_ms(), 200u);
        resp->set_conn_id(202);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertPointTable(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const DLT645Proto::UpsertPointTableRequest &req,
                          DLT645Proto::Empty *) {
        EXPECT_EQ(req.conn_name(), "pt_0A");
        EXPECT_EQ(req.points_size(), 1);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartLink(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyDlt645Config(config, stub.get()));
}

// 验证：CONFIG_PUSHER 会删除 jsonc 未声明的旧链路，并对仍保留且运行中的展开链路先 Stop 再 Upsert，点表按 replace=true 全量覆盖。
TEST(ConfigPusherApplyDlt645Test, ConvergesExpandedLinksToJsoncTargetState) {
  auto config = MakeDlt645Config(false);
  auto stub = std::make_unique<DLT645Proto::MockDLT645ServiceStub>();

  bool listedLinks = false;
  bool deletedLegacyLink = false;
  bool stoppedConv01 = false;
  std::unordered_set<std::string> upsertedConnNames;
  std::unordered_set<std::string> pointTableConnNames;

  InSequence seq;
  EXPECT_CALL(*stub, ListLinks(_, _, _))
      .Times(1)
      .WillOnce(Invoke([&](grpc::ClientContext *,
                           const DLT645Proto::Empty &,
                           DLT645Proto::ListLinksResponse *resp) {
        listedLinks = true;

        auto *legacy = resp->add_links();
        legacy->mutable_config()->set_conn_name("legacy-link");
        legacy->set_state(DLT645Proto::LINK_STATE_STOPPED);

        auto *conv01 = resp->add_links();
        conv01->mutable_config()->set_conn_name("conv_01");
        conv01->set_state(DLT645Proto::LINK_STATE_RUNNING);

        auto *conv0A = resp->add_links();
        conv0A->mutable_config()->set_conn_name("conv_0A");
        conv0A->set_state(DLT645Proto::LINK_STATE_STOPPED);
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, DeleteLink(_, _, _))
      .Times(AtLeast(1))
      .WillRepeatedly(Invoke([&](grpc::ClientContext *,
                                 const DLT645Proto::DeleteLinkRequest &req,
                                 DLT645Proto::Empty *) {
        EXPECT_TRUE(listedLinks);
        if (req.conn_name() == "legacy-link") {
          deletedLegacyLink = true;
        } else {
          ADD_FAILURE() << "DeleteLink 不应删除 jsonc 目标链路: " << req.conn_name();
        }
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, StopLink(_, _, _))
      .Times(AtLeast(1))
      .WillRepeatedly(Invoke([&](grpc::ClientContext *,
                                 const DLT645Proto::StopLinkRequest &req,
                                 DLT645Proto::Empty *) {
        EXPECT_TRUE(listedLinks);
        if (req.conn_name() == "conv_01") {
          stoppedConv01 = true;
        } else if (req.conn_name() != "legacy-link" && req.conn_name() != "conv_0A") {
          ADD_FAILURE() << "StopLink 命中了未知链路: " << req.conn_name();
        }
        return grpc::Status::OK;
      }));

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
      .WillOnce(Invoke([&](grpc::ClientContext *,
                           const DLT645Proto::UpsertLinkRequest &req,
                           DLT645Proto::LinkInfo *resp) {
        EXPECT_EQ(req.config().conn_name(), "conv_01");
        EXPECT_TRUE(stoppedConv01);
        EXPECT_EQ(req.config().device_no(), "01");
        upsertedConnNames.insert(req.config().conn_name());
        resp->set_conn_id(101);
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, UpsertPointTable(_, _, _))
      .WillOnce(Invoke([&](grpc::ClientContext *,
                           const DLT645Proto::UpsertPointTableRequest &req,
                           DLT645Proto::Empty *) {
        EXPECT_TRUE(req.replace());
        EXPECT_EQ(req.points_size(), 1);
        EXPECT_EQ(req.conn_name(), "conv_01");
        EXPECT_TRUE(upsertedConnNames.contains("conv_01"));
        pointTableConnNames.insert(req.conn_name());
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, UpsertLink(_, _, _))
      .WillOnce(Invoke([&](grpc::ClientContext *,
                           const DLT645Proto::UpsertLinkRequest &req,
                           DLT645Proto::LinkInfo *resp) {
        EXPECT_EQ(req.config().conn_name(), "conv_0A");
        EXPECT_EQ(req.config().device_no(), "0A");
        upsertedConnNames.insert(req.config().conn_name());
        resp->set_conn_id(102);
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, UpsertPointTable(_, _, _))
      .WillOnce(Invoke([&](grpc::ClientContext *,
                           const DLT645Proto::UpsertPointTableRequest &req,
                           DLT645Proto::Empty *) {
        EXPECT_TRUE(req.replace());
        EXPECT_EQ(req.points_size(), 1);
        EXPECT_EQ(req.conn_name(), "conv_0A");
        EXPECT_TRUE(upsertedConnNames.contains("conv_0A"));
        pointTableConnNames.insert(req.conn_name());
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, StartLink(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyDlt645Config(config, stub.get()));
  EXPECT_TRUE(deletedLegacyLink);
  EXPECT_TRUE(stoppedConv01);
  EXPECT_EQ(upsertedConnNames.size(), 2u);
  EXPECT_TRUE(upsertedConnNames.contains("conv_01"));
  EXPECT_TRUE(upsertedConnNames.contains("conv_0A"));
  EXPECT_EQ(pointTableConnNames.size(), 2u);
  EXPECT_TRUE(pointTableConnNames.contains("conv_01"));
  EXPECT_TRUE(pointTableConnNames.contains("conv_0A"));
}
