#include "gtest/gtest.h"

#include <boost/json.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

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

  static void SetClientFactory(MQTTManager &mgr, MQTTClientFactory factory) {
    mgr.clientFactory_ = std::move(factory);
  }
};
}  // namespace MQTTManager

namespace {
using MQTTManager::MQTTManagerTestPeer;
using Manager = MQTTManager::MQTTManager;

class FakeMqttClientState {
public:
  void SetRequestResponseTopics(const std::string &requestTopic, const std::string &responseTopic) {
    std::lock_guard<std::mutex> lock(mutex_);
    requestTopic_ = requestTopic;
    responseTopic_ = responseTopic;
  }

  void StartConsuming() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopRequested_ = false;
  }

  void StopConsuming() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopRequested_ = true;
    }
    cv_.notify_all();
  }

  bool IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
  }

  void Connect(const MQTTManager::MQTTClientConnectOptions &options) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      connected_ = true;
      cleanSession_ = options.cleanSession;
      ++connectCount_;
      if (cleanSession_) {
        brokerSubscriptions_.clear();
      }
    }
    cv_.notify_all();
  }

  void Disconnect() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      connected_ = false;
    }
    cv_.notify_all();
  }

  void Subscribe(const std::string &topic, uint32_t qos) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!connected_) {
        throw std::runtime_error("FakeMqttClient 未连接");
      }
      brokerSubscriptions_[topic] = qos;
      ++subscribeCount_;
    }
    cv_.notify_all();
  }

  void Publish(const std::string &topic, const std::string &payload) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!connected_) {
        throw std::runtime_error("FakeMqttClient 未连接");
      }
      ++publishCount_;
      if (topic == requestTopic_ && brokerSubscriptions_.contains(responseTopic_)) {
        consumeQueue_.push_back(MQTTManager::MQTTConsumedMessage{responseTopic_, payload});
      }
    }
    cv_.notify_all();
  }

  std::optional<MQTTManager::MQTTConsumedMessage> ConsumeMessage() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() { return stopRequested_ || !consumeQueue_.empty(); });
    if (consumeQueue_.empty()) {
      return std::nullopt;
    }
    auto next = std::move(consumeQueue_.front());
    consumeQueue_.pop_front();
    return next;
  }

  void ForceDisconnect() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      connected_ = false;
    }
    cv_.notify_all();
  }

  void QueueNullMessage() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      consumeQueue_.push_back(std::nullopt);
    }
    cv_.notify_all();
  }

  bool WaitForConnectCount(size_t expected, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, expected]() { return connectCount_ >= expected; });
  }

  bool WaitForSubscribeCount(size_t expected, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, expected]() { return subscribeCount_ >= expected; });
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool connected_{false};
  bool stopRequested_{false};
  bool cleanSession_{true};
  size_t connectCount_{0};
  size_t subscribeCount_{0};
  size_t publishCount_{0};
  std::string requestTopic_;
  std::string responseTopic_;
  std::unordered_map<std::string, uint32_t> brokerSubscriptions_;
  std::deque<std::optional<MQTTManager::MQTTConsumedMessage>> consumeQueue_;
};

class FakeMqttClient final : public MQTTManager::IMQTTClient {
public:
  explicit FakeMqttClient(std::shared_ptr<FakeMqttClientState> state) : state_(std::move(state)) {}

  void startConsuming() override {
    state_->StartConsuming();
  }

  void stopConsuming() override {
    state_->StopConsuming();
  }

  bool isConnected() const override {
    return state_->IsConnected();
  }

  void connect(const MQTTManager::MQTTClientConnectOptions &options) override {
    state_->Connect(options);
  }

  void disconnect() override {
    state_->Disconnect();
  }

  void publish(const std::string &topic, const std::string &payload, uint32_t qos, bool retain) override {
    (void)qos;
    (void)retain;
    state_->Publish(topic, payload);
  }

  void subscribe(const std::string &topic, uint32_t qos) override {
    state_->Subscribe(topic, qos);
  }

  std::optional<MQTTManager::MQTTConsumedMessage> consumeMessage() override {
    return state_->ConsumeMessage();
  }

private:
  std::shared_ptr<FakeMqttClientState> state_;
};

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

MQTTManagerProto::RequestAndWaitRequest MakeRequestAndWaitRequest(const std::string &clientId,
                                                                 const std::string &payload,
                                                                 bool cleanSession = true,
                                                                 uint32_t timeoutMs = 200) {
  MQTTManagerProto::RequestAndWaitRequest req;
  *req.mutable_connection() = MakeConnectionInfo(clientId, 30, cleanSession, 3000);
  req.set_request_topic("demo/request");
  req.set_response_topic("demo/response");
  req.set_qos(1);
  req.set_retain(false);
  req.set_timeout_ms(timeoutMs);
  req.set_retry_times(0);
  req.set_match_field("id");
  req.set_payload(payload);
  return req;
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

// 验证 clean_session=true 下已缓存的响应主题在 publish 触发重连后会重新订阅。
TEST(MqttManagerReconnectTest, RequestAndWaitRestoresSubscriptionAfterPublishReconnect) {
  Manager mgr;
  auto fakeState = std::make_shared<FakeMqttClientState>();
  fakeState->SetRequestResponseTopics("demo/request", "demo/response");
  MQTTManagerTestPeer::SetClientFactory(
      mgr, [fakeState](const MQTTManagerProto::ConnectionInfo &,
                       const std::string &) -> std::unique_ptr<MQTTManager::IMQTTClient> {
        return std::make_unique<FakeMqttClient>(fakeState);
      });

  auto req = MakeRequestAndWaitRequest("client-a", R"({"id":"token-1"})");
  MQTTManagerProto::RequestAndWaitResponse firstResp;
  auto firstStatus = mgr.RequestAndWait(req, &firstResp);
  ASSERT_TRUE(firstStatus.ok());
  ASSERT_TRUE(firstResp.ok());
  ASSERT_EQ(firstResp.payload(), R"({"id":"token-1"})");
  ASSERT_TRUE(fakeState->WaitForConnectCount(1, std::chrono::milliseconds(200)));
  ASSERT_TRUE(fakeState->WaitForSubscribeCount(1, std::chrono::milliseconds(200)));

  fakeState->ForceDisconnect();

  MQTTManagerProto::PublishRequest publishReq;
  MQTTManagerProto::PublishResponse publishResp;
  *publishReq.mutable_connection() = MakeConnectionInfo("client-a");
  publishReq.set_topic("demo/trigger");
  publishReq.set_qos(1);
  publishReq.set_retain(false);
  publishReq.set_payload("trigger");
  auto publishStatus = mgr.Publish(publishReq, &publishResp);
  ASSERT_TRUE(publishStatus.ok());
  ASSERT_TRUE(publishResp.ok());
  ASSERT_TRUE(fakeState->WaitForConnectCount(2, std::chrono::milliseconds(500)));
  ASSERT_TRUE(fakeState->WaitForSubscribeCount(2, std::chrono::milliseconds(500)));

  req.set_payload(R"({"id":"token-2"})");
  MQTTManagerProto::RequestAndWaitResponse secondResp;
  auto secondStatus = mgr.RequestAndWait(req, &secondResp);
  EXPECT_TRUE(secondStatus.ok());
  EXPECT_TRUE(secondResp.ok());
  EXPECT_EQ(secondResp.payload(), R"({"id":"token-2"})");
}

// 验证 consume_message 返回空消息且底层已断链时，消费线程会触发重连并恢复订阅。
TEST(MqttManagerReconnectTest, ConsumeNullMessageTriggersReconnectAndResubscribe) {
  Manager mgr;
  auto fakeState = std::make_shared<FakeMqttClientState>();
  fakeState->SetRequestResponseTopics("demo/request", "demo/response");
  MQTTManagerTestPeer::SetClientFactory(
      mgr, [fakeState](const MQTTManagerProto::ConnectionInfo &,
                       const std::string &) -> std::unique_ptr<MQTTManager::IMQTTClient> {
        return std::make_unique<FakeMqttClient>(fakeState);
      });

  auto req = MakeRequestAndWaitRequest("client-a", R"({"id":"token-3"})");
  MQTTManagerProto::RequestAndWaitResponse resp;
  auto status = mgr.RequestAndWait(req, &resp);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(resp.ok());
  ASSERT_TRUE(fakeState->WaitForConnectCount(1, std::chrono::milliseconds(200)));
  ASSERT_TRUE(fakeState->WaitForSubscribeCount(1, std::chrono::milliseconds(200)));

  fakeState->ForceDisconnect();
  fakeState->QueueNullMessage();

  EXPECT_TRUE(fakeState->WaitForConnectCount(2, std::chrono::milliseconds(500)));
  EXPECT_TRUE(fakeState->WaitForSubscribeCount(2, std::chrono::milliseconds(500)));
}
