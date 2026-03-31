#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "ConfigPusherApplyModbusRtu.h"
#include "ModbusRTU_mock.grpc.pb.h"

namespace {
using ::testing::_;
using ::testing::AtLeast;
using ::testing::InSequence;
using ::testing::Invoke;

ConfigPusherProto::ModbusRtuConfig MakeMqttModbusConfig(bool start, bool withMqtt) {
  ConfigPusherProto::ModbusRtuConfig config;
  if (withMqtt) {
    auto* mqtt = config.mutable_mqtt();
    mqtt->set_host("127.0.0.1");
    mqtt->set_port(1883);
    mqtt->set_client_id("dlt645");
    mqtt->set_keepalive_sec(30);
    mqtt->set_clean_session(true);
    mqtt->set_connect_timeout_ms(3000);
  }

  auto* task = config.add_links();
  auto* linkCfg = task->mutable_link()->mutable_config();
  linkCfg->set_conn_name("modbus-mqtt-1");
  linkCfg->set_transport_type(ModbusRTUProto::TRANSPORT_MQTT_UART);
  auto* serial = linkCfg->mutable_serial();
  serial->set_baud_rate(9600);
  serial->set_data_bits(8);
  serial->set_parity(ModbusRTUProto::PARITY_NONE);
  serial->set_stop_bits(ModbusRTUProto::STOP_BITS_ONE);
  linkCfg->set_serial_port("RS485-1");
  linkCfg->set_device_id(1);
  linkCfg->set_poll_interval_ms(1000);
  linkCfg->set_request_timeout_ms(3000);
  linkCfg->set_serial_byte_timeout_ms(100);
  linkCfg->set_serial_frame_timeout_ms(100);
  linkCfg->set_serial_est_size(256);

  auto* point = task->mutable_point_table()->add_points();
  point->set_tag("Ua");
  point->set_function(ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS);
  point->set_address(0);
  point->set_type(ModbusRTUProto::DATA_TYPE_UINT16);

  task->set_start(start);
  return config;
}
}  // 命名空间结束

// 验证：ModbusRTU gRPC stub 为空时下发失败。
TEST(ConfigPusherApplyModbusRtuTest, NullStubReturnsFalse) {
  ConfigPusherProto::ModbusRtuConfig config;
  EXPECT_FALSE(ConfigPusher::applyModbusRtuConfig(config, nullptr));
}

// 验证：MQTT UART 链路缺少顶层 mqtt 配置时直接失败，且不会继续下发连接。
TEST(ConfigPusherApplyModbusRtuTest, RejectsMqttLinkWithoutTopLevelMqtt) {
  auto config = MakeMqttModbusConfig(false, false);
  auto stub = std::make_unique<ModbusRTUProto::MockModbusRTUServiceStub>();

  EXPECT_CALL(*stub, ListLinks(_, _, _)).Times(0);
  EXPECT_CALL(*stub, UpdateConfig(_, _, _)).Times(0);
  EXPECT_CALL(*stub, UpsertLink(_, _, _)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyModbusRtuConfig(config, stub.get()));
}

// 验证：存在 MQTT UART 链路时，会先下发 UpdateConfig，再下发连接/点表；配置完成后保持 STOPPED，且不再额外调用 StartLink。
TEST(ConfigPusherApplyModbusRtuTest, AppliesUpdateConfigBeforeLinkWithoutExplicitStart) {
  auto config = MakeMqttModbusConfig(true, true);
  auto stub = std::make_unique<ModbusRTUProto::MockModbusRTUServiceStub>();

  InSequence seq;
  EXPECT_CALL(*stub, ListLinks(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const ModbusRTUProto::Empty&,
                          ModbusRTUProto::ListLinksResponse* resp) {
        resp->Clear();
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpdateConfig(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const ModbusRTUProto::UpdateConfigRequest& req,
                          ModbusRTUProto::UpdateConfigResponse* resp) {
        EXPECT_EQ(req.mqtt().client_id(), "dlt645");
        resp->set_ok(true);
        resp->set_message("成功");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertLink(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const ModbusRTUProto::UpsertLinkRequest& req,
                          ModbusRTUProto::LinkInfo* resp) {
        EXPECT_EQ(req.config().transport_type(), ModbusRTUProto::TRANSPORT_MQTT_UART);
        EXPECT_EQ(req.config().serial_port(), "RS485-1");
        resp->set_conn_id(1001);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertPointTable(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const ModbusRTUProto::UpsertPointTableRequest& req,
                          ModbusRTUProto::Empty*) {
        EXPECT_EQ(req.conn_name(), "modbus-mqtt-1");
        EXPECT_EQ(req.points_size(), 1);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartLink(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyModbusRtuConfig(config, stub.get()));
}

// 验证：MQTT 顶层配置下发失败且链路依赖 MQTT 时，会立即返回失败且不再继续下发连接。
TEST(ConfigPusherApplyModbusRtuTest, UpdateConfigFailureStopsWhenMqttIsRequired) {
  auto config = MakeMqttModbusConfig(false, true);
  auto stub = std::make_unique<ModbusRTUProto::MockModbusRTUServiceStub>();

  EXPECT_CALL(*stub, ListLinks(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const ModbusRTUProto::Empty&,
                          ModbusRTUProto::ListLinksResponse* resp) {
        resp->Clear();
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpdateConfig(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const ModbusRTUProto::UpdateConfigRequest& req,
                          ModbusRTUProto::UpdateConfigResponse*) {
        EXPECT_EQ(req.mqtt().client_id(), "dlt645");
        return grpc::Status(grpc::StatusCode::INTERNAL, "MQTT 下发失败");
      }));
  EXPECT_CALL(*stub, UpsertLink(_, _, _)).Times(0);
  EXPECT_CALL(*stub, StartLink(_, _, _)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyModbusRtuConfig(config, stub.get()));
}

// 验证：CONFIG_PUSHER 会删除 jsonc 未声明的旧链路，并对仍保留且运行中的链路先 Stop 再 Upsert，点表按 replace=true 全量覆盖。
TEST(ConfigPusherApplyModbusRtuTest, ConvergesToJsoncTargetByDeletingStaleLinkAndReplacingPointTable) {
  auto config = MakeMqttModbusConfig(false, true);
  auto stub = std::make_unique<ModbusRTUProto::MockModbusRTUServiceStub>();

  bool listedLinks = false;
  bool deletedLegacyLink = false;
  bool stoppedDesiredLink = false;
  bool upsertedDesiredLink = false;

  InSequence seq;
  EXPECT_CALL(*stub, ListLinks(_, _, _))
      .Times(1)
      .WillOnce(Invoke([&](grpc::ClientContext*,
                           const ModbusRTUProto::Empty&,
                           ModbusRTUProto::ListLinksResponse* resp) {
        listedLinks = true;
        auto* legacy = resp->add_links();
        legacy->mutable_config()->set_conn_name("legacy-link");
        legacy->set_state(ModbusRTUProto::LINK_STATE_STOPPED);

        auto* desired = resp->add_links();
        desired->mutable_config()->set_conn_name("modbus-mqtt-1");
        desired->set_state(ModbusRTUProto::LINK_STATE_RUNNING);
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, DeleteLink(_, _, _))
      .Times(AtLeast(1))
      .WillRepeatedly(Invoke([&](grpc::ClientContext*,
                                 const ModbusRTUProto::DeleteLinkRequest& req,
                                 ModbusRTUProto::Empty*) {
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
      .WillRepeatedly(Invoke([&](grpc::ClientContext*,
                                 const ModbusRTUProto::StopLinkRequest& req,
                                 ModbusRTUProto::Empty*) {
        EXPECT_TRUE(listedLinks);
        if (req.conn_name() == "modbus-mqtt-1") {
          stoppedDesiredLink = true;
        } else if (req.conn_name() != "legacy-link") {
          ADD_FAILURE() << "StopLink 命中了未知链路: " << req.conn_name();
        }
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, UpdateConfig(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const ModbusRTUProto::UpdateConfigRequest& req,
                          ModbusRTUProto::UpdateConfigResponse* resp) {
        EXPECT_EQ(req.mqtt().client_id(), "dlt645");
        resp->set_ok(true);
        resp->set_message("成功");
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, UpsertLink(_, _, _))
      .WillOnce(Invoke([&](grpc::ClientContext*,
                          const ModbusRTUProto::UpsertLinkRequest& req,
                          ModbusRTUProto::LinkInfo* resp) {
        EXPECT_TRUE(stoppedDesiredLink);
        EXPECT_EQ(req.config().conn_name(), "modbus-mqtt-1");
        EXPECT_EQ(req.config().transport_type(), ModbusRTUProto::TRANSPORT_MQTT_UART);
        upsertedDesiredLink = true;
        resp->set_conn_id(1001);
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, UpsertPointTable(_, _, _))
      .WillOnce(Invoke([&](grpc::ClientContext*,
                          const ModbusRTUProto::UpsertPointTableRequest& req,
                          ModbusRTUProto::Empty*) {
        EXPECT_TRUE(upsertedDesiredLink);
        EXPECT_EQ(req.conn_name(), "modbus-mqtt-1");
        EXPECT_EQ(req.points_size(), 1);
        EXPECT_TRUE(req.replace());
        EXPECT_EQ(req.points(0).tag(), "Ua");
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, StartLink(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyModbusRtuConfig(config, stub.get()));
  EXPECT_TRUE(deletedLegacyLink);
  EXPECT_TRUE(stoppedDesiredLink);
}
