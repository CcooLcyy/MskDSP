#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "DLT645MqttClient.h"
#include "MQTTManager_mock.grpc.pb.h"

namespace {
using DLT645::MqttClient;

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

DLT645Proto::MqttConfig MakeConfig() {
  DLT645Proto::MqttConfig cfg;
  cfg.set_host("127.0.0.1");
  cfg.set_port(1883);
  cfg.set_client_id("client-1");
  cfg.set_keepalive_sec(10);
  cfg.set_clean_session(true);
  cfg.set_connect_timeout_ms(1000);
  return cfg;
}
}  // 命名空间结束

// 验证：未配置 MQTT 时拒绝发布。
TEST(Dlt645MqttClientTest, PublishRejectsWithoutConfig) {
  MqttClient client("DLT645");
  std::string error;
  auto st = client.Publish("topic", "payload", &error);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_FALSE(error.empty());
}

// 验证：发布请求字段为空时返回错误。
TEST(Dlt645MqttClientTest, PublishRejectsEmptyFields) {
  MqttClient client("DLT645");
  client.setConfig(MakeConfig());
  auto stub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();
  client.setStub(stub);

  std::string error;
  auto st = client.Publish("", "payload", &error);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(error.empty());
}

// 验证：发布 RPC 失败时透传错误。
TEST(Dlt645MqttClientTest, PublishReturnsRpcError) {
  MqttClient client("DLT645");
  client.setConfig(MakeConfig());
  auto stub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();
  ON_CALL(*stub, Publish(_, _, _))
      .WillByDefault(Return(grpc::Status(grpc::StatusCode::INTERNAL, "RPC 失败")));
  client.setStub(stub);

  std::string error;
  auto st = client.Publish("topic", "payload", &error);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_FALSE(error.empty());
}

// 验证：发布响应 ok=false 时返回内部错误。
TEST(Dlt645MqttClientTest, PublishReturnsResponseError) {
  MqttClient client("DLT645");
  client.setConfig(MakeConfig());
  auto stub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();
  ON_CALL(*stub, Publish(_, _, _))
      .WillByDefault(Invoke([](grpc::ClientContext*, const MQTTManagerProto::PublishRequest&, MQTTManagerProto::PublishResponse* resp) {
        resp->set_ok(false);
        resp->set_message("发布失败");
        return grpc::Status::OK;
      }));
  client.setStub(stub);

  std::string error;
  auto st = client.Publish("topic", "payload", &error);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_FALSE(error.empty());
}

// 验证：发布成功路径返回 OK。
TEST(Dlt645MqttClientTest, PublishSuccess) {
  MqttClient client("DLT645");
  client.setConfig(MakeConfig());
  auto stub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();
  ON_CALL(*stub, Publish(_, _, _))
      .WillByDefault(Invoke([](grpc::ClientContext*, const MQTTManagerProto::PublishRequest&, MQTTManagerProto::PublishResponse* resp) {
        resp->set_ok(true);
        resp->set_message("OK");
        return grpc::Status::OK;
      }));
  client.setStub(stub);

  std::string error;
  auto st = client.Publish("topic", "payload", &error);
  EXPECT_TRUE(st.ok());
  EXPECT_TRUE(error.empty());
}

// 验证：未配置 MQTT 时请求响应失败。
TEST(Dlt645MqttClientTest, RequestAndWaitRejectsWithoutConfig) {
  MqttClient client("DLT645");
  std::string error;
  std::string resp;
  auto st = client.RequestAndWait("req", "resp", "payload", 1000, 0, 0, "token", &resp, &error);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_FALSE(error.empty());
}

// 验证：请求响应字段为空时返回错误。
TEST(Dlt645MqttClientTest, RequestAndWaitRejectsEmptyFields) {
  MqttClient client("DLT645");
  client.setConfig(MakeConfig());
  auto stub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();
  client.setStub(stub);

  std::string error;
  std::string resp;
  auto st = client.RequestAndWait("", "resp", "payload", 1000, 0, 0, "token", &resp, &error);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(error.empty());
}

// 验证：请求响应 RPC 失败时透传错误。
TEST(Dlt645MqttClientTest, RequestAndWaitReturnsRpcError) {
  MqttClient client("DLT645");
  client.setConfig(MakeConfig());
  auto stub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();
  ON_CALL(*stub, RequestAndWait(_, _, _))
      .WillByDefault(Return(grpc::Status(grpc::StatusCode::INTERNAL, "RPC 失败")));
  client.setStub(stub);

  std::string error;
  std::string resp;
  auto st = client.RequestAndWait("req", "resp", "payload", 1000, 0, 0, "token", &resp, &error);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_FALSE(error.empty());
}

// 验证：请求响应 ok=false 时返回内部错误。
TEST(Dlt645MqttClientTest, RequestAndWaitReturnsResponseError) {
  MqttClient client("DLT645");
  client.setConfig(MakeConfig());
  auto stub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();
  ON_CALL(*stub, RequestAndWait(_, _, _))
      .WillByDefault(Invoke([](grpc::ClientContext*, const MQTTManagerProto::RequestAndWaitRequest&, MQTTManagerProto::RequestAndWaitResponse* resp) {
        resp->set_ok(false);
        resp->set_message("响应失败");
        return grpc::Status::OK;
      }));
  client.setStub(stub);

  std::string error;
  std::string resp;
  auto st = client.RequestAndWait("req", "resp", "payload", 1000, 0, 0, "token", &resp, &error);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_FALSE(error.empty());
}

// 验证：请求响应成功时返回 payload。
TEST(Dlt645MqttClientTest, RequestAndWaitSuccess) {
  MqttClient client("DLT645");
  client.setConfig(MakeConfig());
  auto stub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();
  ON_CALL(*stub, RequestAndWait(_, _, _))
      .WillByDefault(Invoke([](grpc::ClientContext*, const MQTTManagerProto::RequestAndWaitRequest&, MQTTManagerProto::RequestAndWaitResponse* resp) {
        resp->set_ok(true);
        resp->set_message("OK");
        resp->set_payload("resp-payload");
        return grpc::Status::OK;
      }));
  client.setStub(stub);

  std::string error;
  std::string resp;
  auto st = client.RequestAndWait("req", "resp", "payload", 1000, 0, 0, "token", &resp, &error);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(resp, "resp-payload");
}
