#include "MQTTManager.h"

#include <boost/dll.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <mqtt/async_client.h>

#include "Logger.h"
#include "MQTTJsonPath.hpp"
#include "MQTTManagerGrpcService.h"
#include "MQTTManagerLibInfo.h"
#include "MQTTTopicMatcher.hpp"
#include "ModuleManager.pb.h"

namespace {
constexpr size_t kPayloadPreviewLen = 256;
constexpr size_t kTopicPreviewLen = 128;
constexpr uint32_t kDefaultTimeoutMs = 3000;
constexpr uint32_t kDefaultRetryIntervalMs = 200;
constexpr uint32_t kDefaultKeepaliveSec = 30;
constexpr uint32_t kDefaultConnectTimeoutMs = 3000;

uint64_t nowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

std::string escapeForLog(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) {
    switch (ch) {
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::string formatPreview(const std::string& text, size_t maxLen) {
  const auto escaped = escapeForLog(text);
  if (escaped.size() <= maxLen) {
    return escaped;
  }
  return escaped.substr(0, maxLen) + "...";
}

std::string formatTopic(const std::string& topic) {
  return formatPreview(topic, kTopicPreviewLen);
}

std::string formatPayload(const std::string& payload) {
  return formatPreview(payload, kPayloadPreviewLen);
}

std::string formatBrokerUri(const MQTTManagerProto::ConnectionInfo& info) {
  std::ostringstream oss;
  oss << "tcp://" << info.host() << ":" << info.port();
  return oss.str();
}

bool parseJsonPayload(const std::string& payload, boost::json::value* out, std::string_view title, bool warnOnly) {
  if (out == nullptr) {
    return false;
  }
  boost::json::parser parser;
  boost::system::error_code ec;
  parser.write(payload, ec);
  if (ec) {
    if (warnOnly) {
      LOG_WARNING("MQTTManager {}解析失败: 错误码={}, 负载={}", title, ec.value(), formatPayload(payload));
    } else {
      LOG_ERROR("MQTTManager {}解析失败: 错误码={}, 负载={}", title, ec.value(), formatPayload(payload));
    }
    return false;
  }
  *out = parser.release();
  return true;
}

std::string makeTopicKey(const std::string& connectionKey, const std::string& topic) {
  std::string key;
  key.reserve(connectionKey.size() + topic.size() + 1);
  key.append(connectionKey);
  key.push_back('|');
  key.append(topic);
  return key;
}

std::string makeMatchKey(const std::string& topicKey, const std::string& matchField,
                         const std::string& matchValueText) {
  std::string key;
  key.reserve(topicKey.size() + matchField.size() + matchValueText.size() + 2);
  key.append(topicKey);
  key.push_back('|');
  key.append(matchField);
  key.push_back('|');
  key.append(matchValueText);
  return key;
}

const std::string& GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(MQTTManagerLibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(MQTTManagerLibInfo.VERSION_MAJOR);
    version->set_minor(MQTTManagerLibInfo.VERSION_MINOR);
    version->set_patch(MQTTManagerLibInfo.VERSION_PATCH);
    version->set_version(MQTTManagerLibInfo.VERSION);
    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace MQTTManager {
struct MQTTManager::Subscriber {
  std::mutex mutex;
  std::condition_variable cv;
  bool stopped{false};
  std::vector<MQTTManagerProto::TopicFilter> filters;
  std::deque<MQTTManagerProto::SubscribeResponse> queue;
};

struct MQTTManager::PendingResponse {
  std::mutex mutex;
  std::condition_variable cv;
  bool done{false};
  std::string payload;
  std::string responseTopic;
  uint64_t recvTimeMs{0};
  std::string requestId;
  std::string matchField;
  std::string matchValueText;
  std::string matchKey;
  std::string topicKey;
  std::vector<JsonPath::Segment> matchPath;
  boost::json::value matchValue;
};

struct MQTTManager::ConnectionContext {
  explicit ConnectionContext(const MQTTManagerProto::ConnectionInfo& info, std::string key)
      : connectionKey(std::move(key)),
        brokerUri(formatBrokerUri(info)),
        clientId(info.client_id()),
        username(info.username()),
        password(info.password()),
        keepaliveSec(info.keepalive_sec()),
        cleanSession(info.clean_session()),
        connectTimeoutMs(info.connect_timeout_ms()),
        client(brokerUri, clientId) {}

  std::string connectionKey;
  std::string brokerUri;
  std::string clientId;
  std::string username;
  std::string password;
  uint32_t keepaliveSec{0};
  bool cleanSession{true};
  uint32_t connectTimeoutMs{0};

  mqtt::async_client client;
  std::atomic<bool> running{false};
  std::thread worker;

  std::mutex subMutex;
  std::unordered_map<std::string, uint32_t> subscriptions;

  std::mutex opMutex;
  std::mutex cbMutex;
  std::function<void(const std::string&, const std::string&)> messageHandler;

  ~ConnectionContext() {
    stop();
  }

  bool start(std::string* error) {
    if (running.load()) {
      return true;
    }
    try {
      client.start_consuming();
    } catch (const std::exception& ex) {
      if (error != nullptr) {
        *error = std::string("启动消费失败: ") + ex.what();
      }
      return false;
    }
    running.store(true);
    worker = std::thread([this]() { consumeLoop(); });
    return true;
  }

  void stop() {
    running.store(false);
    try {
      if (client.is_connected()) {
        client.disconnect()->wait();
      }
    } catch (const std::exception& ex) {
      LOG_WARNING("MQTTManager 断开连接失败: 连接={}, 原因={}", connectionKey, ex.what());
    }
    try {
      client.stop_consuming();
    } catch (const std::exception& ex) {
      LOG_WARNING("MQTTManager 停止消费失败: 连接={}, 原因={}", connectionKey, ex.what());
    }
    if (worker.joinable()) {
      worker.join();
    }
  }

  bool ensureConnected(std::string* error) {
    if (client.is_connected()) {
      return true;
    }
    mqtt::connect_options connOpts;
    connOpts.set_keep_alive_interval(std::chrono::seconds(keepaliveSec > 0 ? keepaliveSec : kDefaultKeepaliveSec));
    connOpts.set_clean_session(cleanSession);
    if (!username.empty()) {
      connOpts.set_user_name(username);
      connOpts.set_password(password);
    }
    connOpts.set_connect_timeout(std::chrono::milliseconds(
        connectTimeoutMs > 0 ? connectTimeoutMs : kDefaultConnectTimeoutMs));
    try {
      client.connect(connOpts)->wait();
      LOG_INFO("MQTTManager 连接成功: 连接={}, 代理={}", connectionKey, brokerUri);
      return true;
    } catch (const std::exception& ex) {
      if (error != nullptr) {
        *error = std::string("连接失败: ") + ex.what();
      }
      LOG_ERROR("MQTTManager 连接失败: 连接={}, 代理={}, 原因={}", connectionKey, brokerUri, ex.what());
      return false;
    }
  }

  bool publish(const std::string& topic, const std::string& payload, uint32_t qos, bool retain, std::string* error) {
    std::lock_guard<std::mutex> lock(opMutex);
    LOG_INFO("MQTTManager 串行化执行发布: 连接={}, 主题={}", connectionKey, formatTopic(topic));
    if (!ensureConnected(error)) {
      return false;
    }
    try {
      auto msg = mqtt::make_message(topic, payload);
      msg->set_qos(static_cast<int>(qos));
      msg->set_retained(retain);
      client.publish(msg)->wait();
      return true;
    } catch (const std::exception& ex) {
      if (error != nullptr) {
        *error = std::string("发布失败: ") + ex.what();
      }
      LOG_ERROR("MQTTManager 发布失败: 连接={}, 主题={}, 原因={}", connectionKey, formatTopic(topic), ex.what());
      return false;
    }
  }

  bool subscribe(const std::string& topic, uint32_t qos, std::string* error) {
    std::lock_guard<std::mutex> lock(subMutex);
    auto it = subscriptions.find(topic);
    if (it != subscriptions.end() && it->second == qos) {
      return true;
    }
    std::lock_guard<std::mutex> opLock(opMutex);
    LOG_INFO("MQTTManager 串行化执行订阅: 连接={}, 主题={}, 质量等级={}", connectionKey, formatTopic(topic), qos);
    if (!ensureConnected(error)) {
      return false;
    }
    try {
      client.subscribe(topic, static_cast<int>(qos))->wait();
      subscriptions[topic] = qos;
      LOG_INFO("MQTTManager 订阅成功: 连接={}, 主题={}, 质量等级={}", connectionKey, formatTopic(topic), qos);
      return true;
    } catch (const std::exception& ex) {
      if (error != nullptr) {
        *error = std::string("订阅失败: ") + ex.what();
      }
      LOG_ERROR("MQTTManager 订阅失败: 连接={}, 主题={}, 原因={}", connectionKey, formatTopic(topic), ex.what());
      return false;
    }
  }

  bool resubscribeAll(std::string* error) {
    std::vector<std::pair<std::string, uint32_t>> topics;
    {
      std::lock_guard<std::mutex> lock(subMutex);
      topics.reserve(subscriptions.size());
      for (const auto& item : subscriptions) {
        topics.emplace_back(item.first, item.second);
      }
    }
    if (topics.empty()) {
      return true;
    }
    std::lock_guard<std::mutex> opLock(opMutex);
    LOG_INFO("MQTTManager 串行化执行重订阅: 连接={}, 主题数量={}", connectionKey, topics.size());
    if (!ensureConnected(error)) {
      return false;
    }
    bool ok = true;
    for (const auto& topic : topics) {
      try {
        client.subscribe(topic.first, static_cast<int>(topic.second))->wait();
        LOG_INFO("MQTTManager 重新订阅成功: 连接={}, 主题={}, 质量等级={}",
                 connectionKey, formatTopic(topic.first), topic.second);
      } catch (const std::exception& ex) {
        ok = false;
        if (error != nullptr) {
          *error = std::string("重订阅失败: ") + ex.what();
        }
        LOG_ERROR("MQTTManager 重新订阅失败: 连接={}, 主题={}, 原因={}",
                  connectionKey, formatTopic(topic.first), ex.what());
      }
    }
    return ok;
  }

  void setMessageHandler(std::function<void(const std::string&, const std::string&)> handler) {
    std::lock_guard<std::mutex> lock(cbMutex);
    messageHandler = std::move(handler);
  }

  void consumeLoop() {
    while (running.load()) {
      try {
        auto msg = client.consume_message();
        if (!msg) {
          if (!running.load()) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }
        std::function<void(const std::string&, const std::string&)> handler;
        {
          std::lock_guard<std::mutex> lock(cbMutex);
          handler = messageHandler;
        }
        if (handler) {
          handler(msg->get_topic(), msg->to_string());
        }
      } catch (const std::exception& ex) {
        LOG_ERROR("MQTTManager 消费线程异常: 连接={}, 原因={}", connectionKey, ex.what());
        if (!running.load()) {
          break;
        }
        std::string error;
        if (!ensureConnected(&error)) {
          LOG_ERROR("MQTTManager 重新连接失败: 连接={}, 原因={}", connectionKey, error);
          std::this_thread::sleep_for(std::chrono::milliseconds(kDefaultRetryIntervalMs));
          continue;
        }
        if (!resubscribeAll(&error)) {
          if (!error.empty()) {
            LOG_WARNING("MQTTManager 重订阅存在失败: 连接={}, 原因={}", connectionKey, error);
          } else {
            LOG_WARNING("MQTTManager 重订阅存在失败: 连接={}", connectionKey);
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kDefaultRetryIntervalMs));
      } catch (...) {
        LOG_ERROR("MQTTManager 消费线程异常: 连接={}, 原因=未知异常", connectionKey);
        if (!running.load()) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kDefaultRetryIntervalMs));
      }
    }
  }
};

MQTTManager::MQTTManager() :
  ModuleInterface(),
  mQTTManagerService_(std::make_shared<MQTTManagerGrpcServiceImpl>()) {
  initLibInfo(MQTTManagerLibInfo);
}

MQTTManager::~MQTTManager() {
  std::lock_guard<std::mutex> lock(connectionMutex_);
  for (auto& [key, ctx] : connections_) {
    ctx->stop();
  }
}

void MQTTManager::start(std::stop_token stopToken) {
  LOG_INFO("MQTTManager 模块启动");
  mQTTManagerService_->setMQTTManager(this);
  LOG_INFO("MQTTManager 服务实例绑定完成");
  grpcServerBuilder(mQTTManagerService_);
  LOG_INFO("MQTTManager gRPC 服务已启动");

  std::mutex mu;
  std::condition_variable_any cv;
  std::stop_callback cb(stopToken, [&cv]() { cv.notify_all(); });
  std::unique_lock lock(mu);
  cv.wait(lock, [&stopToken]() { return stopToken.stop_requested(); });
  LOG_INFO("MQTTManager 模块停止");
}

grpc::Status MQTTManager::Publish(const MQTTManagerProto::PublishRequest& request,
                                  MQTTManagerProto::PublishResponse* response) {
  if (response == nullptr) {
    LOG_ERROR("MQTTManager 发布响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  const auto& conn = request.connection();
  LOG_INFO("MQTTManager 收到发布请求: 代理={}:{}, 主题={}, 质量等级(qos)={}, 保留标志(retain)={}, 负载={}",
           conn.host(), conn.port(), formatTopic(request.topic()), request.qos(), request.retain(),
           formatPayload(request.payload()));
  if (conn.host().empty() || conn.port() == 0 || request.topic().empty() || request.payload().empty()) {
    response->set_ok(false);
    response->set_message("连接参数或主题或负载为空");
    LOG_ERROR("MQTTManager 发布失败: 连接参数或主题或负载为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "连接参数或主题或负载为空");
  }

  std::string error;
  auto ctx = getOrCreateConnection(conn, &error);
  if (!ctx) {
    response->set_ok(false);
    response->set_message(error);
    LOG_ERROR("MQTTManager 发布失败: 连接准备失败, 原因={}", error);
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, error);
  }

  if (!ctx->publish(request.topic(), request.payload(), request.qos(), request.retain(), &error)) {
    response->set_ok(false);
    response->set_message(error);
    LOG_ERROR("MQTTManager 发布失败: {}", error);
    return grpc::Status(grpc::StatusCode::INTERNAL, error);
  }

  response->set_ok(true);
  response->set_message("发布成功");
  LOG_INFO("MQTTManager 发布完成: 主题={}, 负载={}", formatTopic(request.topic()),
           formatPayload(request.payload()));
  return grpc::Status::OK;
}

grpc::Status MQTTManager::Subscribe(const MQTTManagerProto::SubscribeRequest& request,
                                    grpc::ServerWriter<MQTTManagerProto::SubscribeResponse>* writer,
                                    grpc::ServerContext* context) {
  if (writer == nullptr || context == nullptr) {
    LOG_ERROR("MQTTManager 订阅响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  const auto& conn = request.connection();
  LOG_INFO("MQTTManager 收到订阅请求: 代理={}:{}, 主题数量={}", conn.host(), conn.port(), request.topics_size());
  if (conn.host().empty() || conn.port() == 0) {
    LOG_ERROR("MQTTManager 订阅失败: 连接参数为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "连接参数为空");
  }
  if (request.topics().empty()) {
    LOG_ERROR("MQTTManager 订阅失败: 订阅主题为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "订阅主题为空");
  }

  std::string error;
  auto ctx = getOrCreateConnection(conn, &error);
  if (!ctx) {
    LOG_ERROR("MQTTManager 订阅失败: 连接准备失败, 原因={}", error);
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, error);
  }

  for (const auto& filter : request.topics()) {
    if (filter.topic().empty()) {
      LOG_ERROR("MQTTManager 订阅失败: 订阅主题为空");
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "订阅主题为空");
    }
    if (!ctx->subscribe(filter.topic(), filter.qos(), &error)) {
      LOG_ERROR("MQTTManager 订阅失败: {}", error);
      return grpc::Status(grpc::StatusCode::INTERNAL, error);
    }
  }

  auto subscriber = std::make_shared<Subscriber>();
  for (const auto& filter : request.topics()) {
    subscriber->filters.push_back(filter);
  }

  {
    std::lock_guard<std::mutex> lock(subscriberMutex_);
    subscribers_[ctx->connectionKey].insert(subscriber);
  }

  LOG_INFO("MQTTManager 订阅建立完成: 代理={}:{}, 主题数量={}", conn.host(), conn.port(), request.topics_size());

  while (!context->IsCancelled()) {
    MQTTManagerProto::SubscribeResponse msg;
    {
      std::unique_lock<std::mutex> lock(subscriber->mutex);
      subscriber->cv.wait(lock, [&subscriber]() { return subscriber->stopped || !subscriber->queue.empty(); });
      if (subscriber->stopped) {
        break;
      }
      msg = std::move(subscriber->queue.front());
      subscriber->queue.pop_front();
    }
    if (!writer->Write(msg)) {
      LOG_WARNING("MQTTManager 订阅写入失败: 代理={}:{}, 主题={}", conn.host(), conn.port(), formatTopic(msg.topic()));
      break;
    }
  }

  {
    std::lock_guard<std::mutex> lock(subscriberMutex_);
    auto it = subscribers_.find(ctx->connectionKey);
    if (it != subscribers_.end()) {
      it->second.erase(subscriber);
    }
  }
  {
    std::lock_guard<std::mutex> lock(subscriber->mutex);
    subscriber->stopped = true;
  }
  subscriber->cv.notify_all();
  LOG_INFO("MQTTManager 订阅结束: 代理={}:{}, 主题数量={}", conn.host(), conn.port(), request.topics_size());
  return grpc::Status::OK;
}

grpc::Status MQTTManager::RequestAndWait(const MQTTManagerProto::RequestAndWaitRequest& request,
                                         MQTTManagerProto::RequestAndWaitResponse* response) {
  if (response == nullptr) {
    LOG_ERROR("MQTTManager 请求响应响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }

  const auto& conn = request.connection();
  LOG_INFO("MQTTManager 收到请求响应请求: 代理={}:{}, 请求主题={}, 响应主题={}, 匹配字段={}, 负载={}", conn.host(),
           conn.port(), formatTopic(request.request_topic()), formatTopic(request.response_topic()),
           request.match_field(), formatPayload(request.payload()));

  if (conn.host().empty() || conn.port() == 0 || request.request_topic().empty() ||
      request.response_topic().empty() || request.payload().empty()) {
    response->set_ok(false);
    response->set_message("连接参数或主题或负载为空");
    LOG_ERROR("MQTTManager 请求响应失败: 连接参数或主题或负载为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "连接参数或主题或负载为空");
  }
  if (request.match_field().empty()) {
    response->set_ok(false);
    response->set_message("匹配字段为空");
    LOG_ERROR("MQTTManager 请求响应失败: 匹配字段为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "匹配字段为空");
  }

  boost::json::value requestJson;
  if (!parseJsonPayload(request.payload(), &requestJson, "请求负载", false)) {
    response->set_ok(false);
    response->set_message("负载不是合法JSON");
    LOG_ERROR("MQTTManager 请求响应失败: 负载不是合法JSON");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "负载不是合法JSON");
  }

  std::vector<JsonPath::Segment> matchPath;
  std::string matchPathError;
  if (!JsonPath::parsePath(request.match_field(), &matchPath, &matchPathError)) {
    response->set_ok(false);
    response->set_message("匹配字段路径无效");
    LOG_ERROR("MQTTManager 请求响应失败: 匹配字段路径无效, 字段={}, 原因={}", request.match_field(), matchPathError);
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "匹配字段路径无效");
  }

  const boost::json::value* matchValue = nullptr;
  std::string matchValueError;
  if (!JsonPath::extractValue(requestJson, matchPath, &matchValue, &matchValueError)) {
    response->set_ok(false);
    response->set_message("负载缺少匹配字段");
    LOG_ERROR("MQTTManager 请求响应失败: 负载缺少匹配字段, 字段={}, 原因={}", request.match_field(), matchValueError);
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "负载缺少匹配字段");
  }
  const std::string matchValueText = boost::json::serialize(*matchValue);

  std::string error;
  auto ctx = getOrCreateConnection(conn, &error);
  if (!ctx) {
    response->set_ok(false);
    response->set_message(error);
    LOG_ERROR("MQTTManager 请求响应失败: 连接准备失败, 原因={}", error);
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, error);
  }

  if (!ctx->subscribe(request.response_topic(), request.qos(), &error)) {
    response->set_ok(false);
    response->set_message(error);
    LOG_ERROR("MQTTManager 请求响应失败: 响应订阅失败, 原因={}", error);
    return grpc::Status(grpc::StatusCode::INTERNAL, error);
  }

  const uint32_t timeoutMs = request.timeout_ms() > 0 ? request.timeout_ms() : kDefaultTimeoutMs;
  const uint32_t retryTimes = request.retry_times();
  const uint32_t retryIntervalMs =
      request.retry_interval_ms() > 0 ? request.retry_interval_ms() : kDefaultRetryIntervalMs;
  std::string requestId = request.request_id().empty() ? generateRequestId() : request.request_id();

  auto pending = std::make_shared<PendingResponse>();
  pending->requestId = requestId;
  pending->matchField = request.match_field();
  pending->matchValue = *matchValue;
  pending->matchValueText = matchValueText;
  pending->matchPath = std::move(matchPath);
  pending->topicKey = makeTopicKey(ctx->connectionKey, request.response_topic());
  pending->matchKey = makeMatchKey(pending->topicKey, pending->matchField, pending->matchValueText);
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    if (pendingMatchKeys_.find(pending->matchKey) != pendingMatchKeys_.end()) {
      response->set_ok(false);
      response->set_message("匹配条件冲突");
      LOG_ERROR("MQTTManager 请求响应失败: 匹配条件冲突, 连接={}, 响应主题={}, 匹配字段={}, 匹配值={}",
                ctx->connectionKey, formatTopic(request.response_topic()), pending->matchField,
                pending->matchValueText);
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "匹配条件冲突");
    }
    pendingMatchKeys_.insert(pending->matchKey);
    pendingByTopic_[pending->topicKey].insert(pending);
  }

  bool done = false;
  std::string lastError;
  for (uint32_t attempt = 0; attempt <= retryTimes; ++attempt) {
    if (!ctx->publish(request.request_topic(), request.payload(), request.qos(), request.retain(), &error)) {
      lastError = error;
      LOG_ERROR("MQTTManager 请求响应发布失败: 尝试次数={}, 原因={}", attempt + 1, error);
    } else {
      LOG_INFO("MQTTManager 请求响应已发送: 请求ID={}, 主题={}, 匹配字段={}, 匹配值={}, 负载={}", requestId,
               formatTopic(request.request_topic()), request.match_field(), matchValueText,
               formatPayload(request.payload()));
    }

    std::unique_lock<std::mutex> lock(pending->mutex);
    if (pending->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                             [&pending]() { return pending->done; })) {
      done = true;
      break;
    }
    if (attempt < retryTimes) {
      LOG_WARNING("MQTTManager 请求响应等待超时，准备重试: 请求ID={}, 重试次数={}", requestId, attempt + 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
    }
  }

  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingMatchKeys_.erase(pending->matchKey);
    auto it = pendingByTopic_.find(pending->topicKey);
    if (it != pendingByTopic_.end()) {
      it->second.erase(pending);
      if (it->second.empty()) {
        pendingByTopic_.erase(it);
      }
    }
  }

  if (!done) {
    response->set_ok(false);
    response->set_message("等待响应超时");
    response->set_request_id(requestId);
    LOG_ERROR("MQTTManager 请求响应超时: 请求ID={}, 最后错误={}", requestId, lastError);
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "等待响应超时");
  }

  response->set_request_id(requestId);
  response->set_response_topic(pending->responseTopic);
  response->set_recv_time_ms(pending->recvTimeMs);
  response->set_payload(pending->payload);

  response->set_ok(true);
  response->set_message("响应成功");
  LOG_INFO("MQTTManager 请求响应成功: 请求ID={}, 响应主题={}, 匹配字段={}, 匹配值={}, 负载={}", requestId,
           formatTopic(pending->responseTopic), pending->matchField, pending->matchValueText,
           formatPayload(pending->payload));
  return grpc::Status::OK;
}

std::shared_ptr<MQTTManager::ConnectionContext> MQTTManager::getOrCreateConnection(
    const MQTTManagerProto::ConnectionInfo& info, std::string* error) {
  const auto key = makeConnectionKey(info);
  {
    std::lock_guard<std::mutex> lock(connectionMutex_);
    auto it = connections_.find(key);
    if (it != connections_.end()) {
      const auto& ctx = it->second;
      if (ctx->clientId != info.client_id() || ctx->username != info.username() || ctx->password != info.password() ||
          ctx->keepaliveSec != info.keepalive_sec() || ctx->cleanSession != info.clean_session() ||
          ctx->connectTimeoutMs != info.connect_timeout_ms()) {
        if (error != nullptr) {
          *error = "连接参数冲突，请使用同一连接参数";
        }
        return nullptr;
      }
      return ctx;
    }
  }

  auto context = std::make_shared<ConnectionContext>(info, key);
  if (!context->start(error)) {
    return nullptr;
  }
  context->setMessageHandler([this, key](const std::string& topic, const std::string& payload) {
    handleIncomingMessage(key, topic, payload);
  });

  std::shared_ptr<ConnectionContext> existing;
  {
    std::lock_guard<std::mutex> lock(connectionMutex_);
    auto [it, inserted] = connections_.emplace(key, context);
    if (!inserted) {
      existing = it->second;
    }
  }
  if (existing) {
    LOG_WARNING("MQTTManager 连接已存在，丢弃重复创建: 连接={}", key);
    context->stop();
    return existing;
  }
  return context;
}

std::string MQTTManager::makeConnectionKey(const MQTTManagerProto::ConnectionInfo& info) const {
  std::ostringstream oss;
  oss << info.host() << ":" << info.port();
  return oss.str();
}

std::string MQTTManager::generateRequestId() {
  const auto count = requestCounter_.fetch_add(1) + 1;
  std::ostringstream oss;
  oss << "req-" << nowMs() << "-" << count;
  return oss.str();
}

void MQTTManager::handleIncomingMessage(const std::string& connectionKey, const std::string& topic,
                                        const std::string& payload) {
  LOG_INFO("MQTTManager 收到消息: 连接={}, 主题={}, 负载={}", connectionKey, formatTopic(topic),
           formatPayload(payload));

  std::vector<std::shared_ptr<PendingResponse>> candidates;
  const auto topicKey = makeTopicKey(connectionKey, topic);
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    auto it = pendingByTopic_.find(topicKey);
    if (it != pendingByTopic_.end()) {
      candidates.assign(it->second.begin(), it->second.end());
    }
  }

  if (!candidates.empty()) {
    boost::json::value responseJson;
    if (parseJsonPayload(payload, &responseJson, "响应负载", true)) {
      for (const auto& pending : candidates) {
        const boost::json::value* matchedValue = nullptr;
        std::string matchError;
        if (!JsonPath::extractValue(responseJson, pending->matchPath, &matchedValue, &matchError)) {
          continue;
        }
        if (*matchedValue == pending->matchValue) {
          {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->done = true;
            pending->payload = payload;
            pending->responseTopic = topic;
            pending->recvTimeMs = nowMs();
          }
          pending->cv.notify_all();
          LOG_INFO("MQTTManager 响应匹配成功: 连接={}, 请求ID={}, 匹配字段={}, 匹配值={}", connectionKey,
                   pending->requestId, pending->matchField, pending->matchValueText);
          return;
        }
      }
      LOG_INFO("MQTTManager 响应未匹配等待请求，转发订阅处理: 连接={}, 主题={}", connectionKey, formatTopic(topic));
    } else {
      LOG_INFO("MQTTManager 响应负载解析失败，转发订阅处理: 连接={}, 主题={}", connectionKey, formatTopic(topic));
    }
  }

  dispatchToSubscribers(connectionKey, topic, payload);
}

void MQTTManager::dispatchToSubscribers(const std::string& connectionKey, const std::string& topic,
                                        const std::string& payload) {
  std::unordered_set<std::shared_ptr<Subscriber>> subscribers;
  {
    std::lock_guard<std::mutex> lock(subscriberMutex_);
    auto it = subscribers_.find(connectionKey);
    if (it != subscribers_.end()) {
      subscribers = it->second;
    }
  }

  if (subscribers.empty()) {
    return;
  }

  for (const auto& subscriber : subscribers) {
    bool matched = false;
    for (const auto& filter : subscriber->filters) {
      if (MatchTopicFilter(filter.topic(), topic)) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      continue;
    }
    MQTTManagerProto::SubscribeResponse response;
    response.set_topic(topic);
    response.set_payload(payload);
    response.set_recv_time_ms(nowMs());
    {
      std::lock_guard<std::mutex> lock(subscriber->mutex);
      subscriber->queue.push_back(response);
    }
    subscriber->cv.notify_all();
  }
}
}  // namespace MQTTManager

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new MQTTManager::MQTTManager();
}

extern "C" BOOST_SYMBOL_EXPORT bool GetModuleManifestPb(const uint8_t** data, size_t* size) {
  if (data == nullptr || size == nullptr) {
    return false;
  }
  const auto& serialized = GetSerializedManifest();
  *data = reinterpret_cast<const uint8_t*>(serialized.data());
  *size = serialized.size();
  return true;
}
