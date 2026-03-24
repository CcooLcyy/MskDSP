#include "gtest/gtest.h"

#include <boost/json.hpp>

#include <string>

#include "MQTTJsonPath.hpp"
#include "MQTTManager.h"
#include "MQTTManager.pb.h"
#include "MQTTTopicMatcher.hpp"

namespace MQTTManager {
class MQTTManagerTestPeer {
public:
  static std::string MakeConnectionKey(MQTTManager &mgr, const MQTTManagerProto::ConnectionInfo &info) {
    return mgr.makeConnectionKey(info);
  }

  static bool PrepareConnection(MQTTManager &mgr, const MQTTManagerProto::ConnectionInfo &info, std::string *error) {
    return static_cast<bool>(mgr.getOrCreateConnection(info, error));
  }

  static size_t ConnectionCount(MQTTManager &mgr) { return mgr.connections_.size(); }
};
}  // namespace MQTTManager

namespace {
using MQTTManager::MQTTManagerTestPeer;

MQTTManagerProto::ConnectionInfo MakeConnectionInfo(const std::string &clientId,
                                                    uint32_t keepaliveSec = 30,
                                                    bool cleanSession = true,
                                                    uint32_t connectTimeoutMs = 3000,
                                                    const std::string &username = "",
                                                    const std::string &password = "") {
  MQTTManagerProto::ConnectionInfo info;
  info.set_host("127.0.0.1");
  info.set_port(1883);
  info.set_client_id(clientId);
  info.set_username(username);
  info.set_password(password);
  info.set_keepalive_sec(keepaliveSec);
  info.set_clean_session(cleanSession);
  info.set_connect_timeout_ms(connectTimeoutMs);
  return info;
}
}  // namespace

// 验证主题过滤匹配支持 + 与 # 通配符。
TEST(MqttTopicMatcherTest, MatchTopicFilterSupportsWildcards) {
  EXPECT_TRUE(MQTTManager::MatchTopicFilter("#", "a/b/c"));
  EXPECT_TRUE(MQTTManager::MatchTopicFilter("a/+/c", "a/b/c"));
  EXPECT_FALSE(MQTTManager::MatchTopicFilter("a/+/c", "a/b/d"));
  EXPECT_TRUE(MQTTManager::MatchTopicFilter("a/#", "a/b/c"));
  EXPECT_TRUE(MQTTManager::MatchTopicFilter("a/b", "a/b"));
  EXPECT_FALSE(MQTTManager::MatchTopicFilter("a/b", "a/b/c"));
}

// 验证嵌套路径能够正确解析并提取字段值。
TEST(MqttJsonPathTest, ExtractNestedField) {
  boost::system::error_code ec;
  const auto json = boost::json::parse(
      R"({"data":{"items":[{"id":"a1"},{"id":"b2"}]}})", ec);
  ASSERT_FALSE(ec);

  std::vector<MQTTManager::JsonPath::Segment> segments;
  std::string error;
  ASSERT_TRUE(MQTTManager::JsonPath::parsePath("data.items[1].id", &segments, &error));

  const boost::json::value* value = nullptr;
  ASSERT_TRUE(MQTTManager::JsonPath::extractValue(json, segments, &value, &error));
  ASSERT_TRUE(value->is_string());
  EXPECT_EQ(value->as_string(), "b2");
}

// 验证根数组路径解析与字段提取。
TEST(MqttJsonPathTest, ExtractFromRootArray) {
  boost::system::error_code ec;
  const auto json = boost::json::parse(
      R"([{"id":"x"},{"id":"y"}])", ec);
  ASSERT_FALSE(ec);

  std::vector<MQTTManager::JsonPath::Segment> segments;
  std::string error;
  ASSERT_TRUE(MQTTManager::JsonPath::parsePath("[0].id", &segments, &error));

  const boost::json::value* value = nullptr;
  ASSERT_TRUE(MQTTManager::JsonPath::extractValue(json, segments, &value, &error));
  ASSERT_TRUE(value->is_string());
  EXPECT_EQ(value->as_string(), "x");
}

// 验证非法路径会解析失败。
TEST(MqttJsonPathTest, RejectsInvalidPath) {
  std::vector<MQTTManager::JsonPath::Segment> segments;
  std::string error;
  EXPECT_FALSE(MQTTManager::JsonPath::parsePath("", &segments, &error));
  EXPECT_FALSE(MQTTManager::JsonPath::parsePath("data..id", &segments, &error));
  EXPECT_FALSE(MQTTManager::JsonPath::parsePath("data[", &segments, &error));
}

// 验证路径存在但字段缺失时返回失败。
TEST(MqttJsonPathTest, MissingFieldReturnsFalse) {
  boost::system::error_code ec;
  const auto json = boost::json::parse(
      R"({"data":{"id":"v1"}})", ec);
  ASSERT_FALSE(ec);

  std::vector<MQTTManager::JsonPath::Segment> segments;
  std::string error;
  ASSERT_TRUE(MQTTManager::JsonPath::parsePath("data.missing", &segments, &error));

  const boost::json::value* value = nullptr;
  EXPECT_FALSE(MQTTManager::JsonPath::extractValue(json, segments, &value, &error));
}

// 验证连接键包含 client_id，避免同一 broker 下不同客户端相互冲突。
TEST(MqttManagerConnectionReuseTest, ConnectionKeyIncludesClientId) {
  MQTTManager::MQTTManager mgr;
  const auto connA = MakeConnectionInfo("client-a");
  const auto connB = MakeConnectionInfo("client-b");

  EXPECT_NE(MQTTManagerTestPeer::MakeConnectionKey(mgr, connA),
            MQTTManagerTestPeer::MakeConnectionKey(mgr, connB));
}

// 验证同一 broker 下不同 client_id 可建立独立连接并并存。
TEST(MqttManagerConnectionReuseTest, AllowsSameBrokerWithDifferentClientId) {
  MQTTManager::MQTTManager mgr;
  std::string error;

  EXPECT_TRUE(MQTTManagerTestPeer::PrepareConnection(mgr, MakeConnectionInfo("client-a"), &error));
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(MQTTManagerTestPeer::PrepareConnection(mgr, MakeConnectionInfo("client-b"), &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(MQTTManagerTestPeer::ConnectionCount(mgr), 2u);
}

// 验证同一 broker 与同一 client_id 下其余连接参数不一致时仍拒绝复用。
TEST(MqttManagerConnectionReuseTest, RejectsSameBrokerSameClientIdWithDifferentOptions) {
  MQTTManager::MQTTManager mgr;
  std::string error;

  EXPECT_TRUE(MQTTManagerTestPeer::PrepareConnection(mgr, MakeConnectionInfo("client-a", 30, true, 3000), &error));
  EXPECT_TRUE(error.empty());

  error.clear();
  EXPECT_FALSE(MQTTManagerTestPeer::PrepareConnection(mgr, MakeConnectionInfo("client-a", 60, true, 3000), &error));
  EXPECT_EQ(error, "连接参数冲突，请使用同一连接参数");
  EXPECT_EQ(MQTTManagerTestPeer::ConnectionCount(mgr), 1u);
}

// 验证：Publish 参数校验分支。
TEST(MqttManagerServiceTest, PublishRejectsInvalidArgs) {
  MQTTManager::MQTTManager mgr;
  MQTTManagerProto::PublishRequest req;
  MQTTManagerProto::PublishResponse resp;
  auto st = mgr.Publish(req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(resp.ok());
}

// 验证：Subscribe 空指针时返回错误。
TEST(MqttManagerServiceTest, SubscribeRejectsNullWriter) {
  MQTTManager::MQTTManager mgr;
  MQTTManagerProto::SubscribeRequest req;
  grpc::ServerContext ctx;
  auto st = mgr.Subscribe(req, nullptr, &ctx);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：RequestAndWait 入参校验分支。
TEST(MqttManagerServiceTest, RequestAndWaitRejectsInvalidArgs) {
  MQTTManager::MQTTManager mgr;
  MQTTManagerProto::RequestAndWaitRequest req;
  MQTTManagerProto::RequestAndWaitResponse resp;
  auto st = mgr.RequestAndWait(req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(resp.ok());
}

// 验证：RequestAndWait 非法 JSON 负载返回错误。
TEST(MqttManagerServiceTest, RequestAndWaitRejectsInvalidJson) {
  MQTTManager::MQTTManager mgr;
  MQTTManagerProto::RequestAndWaitRequest req;
  auto *conn = req.mutable_connection();
  conn->set_host("127.0.0.1");
  conn->set_port(1883);
  req.set_request_topic("req");
  req.set_response_topic("resp");
  req.set_payload("{bad_json");
  req.set_match_field("id");

  MQTTManagerProto::RequestAndWaitResponse resp;
  auto st = mgr.RequestAndWait(req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(resp.ok());
}

// 验证：RequestAndWait 匹配字段路径无效时返回错误。
TEST(MqttManagerServiceTest, RequestAndWaitRejectsInvalidMatchPath) {
  MQTTManager::MQTTManager mgr;
  MQTTManagerProto::RequestAndWaitRequest req;
  auto *conn = req.mutable_connection();
  conn->set_host("127.0.0.1");
  conn->set_port(1883);
  req.set_request_topic("req");
  req.set_response_topic("resp");
  req.set_payload(R"({"id":1})");
  req.set_match_field("..id");

  MQTTManagerProto::RequestAndWaitResponse resp;
  auto st = mgr.RequestAndWait(req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(resp.ok());
}

// 验证：RequestAndWait 缺少匹配字段时返回错误。
TEST(MqttManagerServiceTest, RequestAndWaitRejectsMissingMatchField) {
  MQTTManager::MQTTManager mgr;
  MQTTManagerProto::RequestAndWaitRequest req;
  auto *conn = req.mutable_connection();
  conn->set_host("127.0.0.1");
  conn->set_port(1883);
  req.set_request_topic("req");
  req.set_response_topic("resp");
  req.set_payload(R"({"other":1})");
  req.set_match_field("id");

  MQTTManagerProto::RequestAndWaitResponse resp;
  auto st = mgr.RequestAndWait(req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(resp.ok());
}
