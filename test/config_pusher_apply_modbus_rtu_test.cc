#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include "ConfigPusherApplyModbusRtu.h"
#include "ModbusRTU_mock.grpc.pb.h"

namespace {
using ::testing::_;
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

  EXPECT_CALL(*stub, UpdateConfig(_, _, _)).Times(0);
  EXPECT_CALL(*stub, UpsertLink(_, _, _)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyModbusRtuConfig(config, stub.get()));
}

// 验证：存在 MQTT UART 链路时，会先下发 UpdateConfig，再下发连接/点表/启动连接。
TEST(ConfigPusherApplyModbusRtuTest, AppliesUpdateConfigBeforeLinkAndStart) {
  auto config = MakeMqttModbusConfig(true, true);
  auto stub = std::make_unique<ModbusRTUProto::MockModbusRTUServiceStub>();

  InSequence seq;
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
  EXPECT_CALL(*stub, StartLink(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const ModbusRTUProto::StartLinkRequest& req,
                          ModbusRTUProto::Empty*) {
        EXPECT_EQ(req.conn_name(), "modbus-mqtt-1");
        return grpc::Status::OK;
      }));

  EXPECT_TRUE(ConfigPusher::applyModbusRtuConfig(config, stub.get()));
}
