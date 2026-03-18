#include "DLT645MqttClient.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <utility>

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include "Logger.h"

namespace DLT645 {
namespace {
constexpr const char* kDefaultModuleName = "MQTTManager";
}  // namespace

MqttClient::MqttClient(std::string moduleName) :
  moduleName_(std::move(moduleName)),
  serverAddress_(buildUnixSocketAddress()) {}

void MqttClient::setConfig(const DLT645Proto::MqttConfig& config) {
  std::lock_guard<std::mutex> lock(mu_);
  config_ = config;
  hasConfig_ = true;
  if (hasInjectedStub_ && injectedStub_) {
    stub_ = injectedStub_;
    channel_.reset();
    LOG_INFO("DLT645 MQTT 配置已更新，保留已注入 Stub 供后续请求继续复用");
    return;
  }
  channel_.reset();
  stub_.reset();
  LOG_INFO("DLT645 MQTT 配置已更新，已清理 MQTT 连接缓存，后续请求将重建真实 Stub");
}

void MqttClient::setStub(std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface> stub) {
  std::lock_guard<std::mutex> lock(mu_);
  injectedStub_ = std::move(stub);
  hasInjectedStub_ = static_cast<bool>(injectedStub_);
  stub_ = injectedStub_;
  channel_.reset();
  if (hasInjectedStub_) {
    LOG_INFO("DLT645 MQTT Stub 已设置，后续请求将优先使用注入 Stub");
  } else {
    LOG_INFO("DLT645 MQTT Stub 已清除，后续请求将使用真实连接");
  }
}

bool MqttClient::hasConfig() const {
  std::lock_guard<std::mutex> lock(mu_);
  return hasConfig_;
}

grpc::Status MqttClient::Publish(const std::string& topic, const std::string& payload, std::string* error) {
  LOG_INFO("DLT645 MQTT 发布入口: 主题={}, 负载长度={}", topic, payload.size());
  if (!hasConfig()) {
    if (error != nullptr) {
      *error = "MQTT 连接参数未配置";
    }
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "MQTT 连接参数未配置");
  }
  if (topic.empty()) {
    if (error != nullptr) {
      *error = "主题为空";
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "主题为空");
  }
  if (payload.empty()) {
    if (error != nullptr) {
      *error = "负载为空";
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "负载为空");
  }

  LOG_INFO("DLT645 准备 MQTT 发布: 连接={}, 主题={}, 负载长度={}", connectionLabel(), topic, payload.size());

  auto stub = getStub();
  MQTTManagerProto::PublishRequest req;
  *req.mutable_connection() = makeConnection();
  req.set_topic(topic);
  req.set_qos(1);
  req.set_retain(false);
  req.set_payload(payload);

  MQTTManagerProto::PublishResponse resp;
  grpc::ClientContext ctx;
  const auto timeoutMs = config_.connect_timeout_ms() > 0 ? config_.connect_timeout_ms() : 3000;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(timeoutMs));
  auto status = stub->Publish(&ctx, req, &resp);
  if (!status.ok()) {
    if (error != nullptr) {
      *error = status.error_message();
    }
    LOG_ERROR("DLT645 MQTT 发布 RPC 失败: 连接={}, 主题={}, 原因={}", connectionLabel(), topic, status.error_message());
    return status;
  }
  if (!resp.ok()) {
    if (error != nullptr) {
      *error = resp.message();
    }
    LOG_ERROR("DLT645 MQTT 发布失败: 连接={}, 主题={}, 原因={}", connectionLabel(), topic, resp.message());
    return grpc::Status(grpc::StatusCode::INTERNAL, resp.message());
  }
  LOG_INFO("DLT645 MQTT 发布成功: 连接={}, 主题={}, 负载长度={}", connectionLabel(), topic, payload.size());
  return grpc::Status::OK;
}

grpc::Status MqttClient::RequestAndWait(const std::string& requestTopic,
                                        const std::string& responseTopic,
                                        const std::string& payload,
                                        uint32_t timeoutMs,
                                        uint32_t retryTimes,
                                        uint32_t retryIntervalMs,
                                        const std::string& matchField,
                                        std::string* outResponsePayload,
                                        std::string* error) {
  LOG_INFO("DLT645 MQTT 请求响应入口: 请求主题={}, 响应主题={}, 负载长度={}", requestTopic, responseTopic,
           payload.size());
  if (!hasConfig()) {
    if (error != nullptr) {
      *error = "MQTT 连接参数未配置";
    }
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "MQTT 连接参数未配置");
  }
  if (requestTopic.empty() || responseTopic.empty()) {
    if (error != nullptr) {
      *error = "主题为空";
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "主题为空");
  }
  if (payload.empty()) {
    if (error != nullptr) {
      *error = "负载为空";
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "负载为空");
  }
  if (matchField.empty()) {
    if (error != nullptr) {
      *error = "匹配字段为空";
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "匹配字段为空");
  }

  LOG_INFO("DLT645 准备 MQTT 请求响应: 连接={}, 请求主题={}, 响应主题={}, 负载长度={}",
           connectionLabel(), requestTopic, responseTopic, payload.size());

  auto stub = getStub();
  MQTTManagerProto::RequestAndWaitRequest req;
  *req.mutable_connection() = makeConnection();
  req.set_request_topic(requestTopic);
  req.set_response_topic(responseTopic);
  req.set_payload(payload);
  req.set_qos(1);
  req.set_retain(false);
  req.set_timeout_ms(timeoutMs);
  req.set_retry_times(retryTimes);
  req.set_retry_interval_ms(retryIntervalMs);
  req.set_match_field(matchField);

  MQTTManagerProto::RequestAndWaitResponse resp;
  grpc::ClientContext ctx;
  const uint32_t effectiveTimeoutMs = timeoutMs > 0 ? timeoutMs : 3000;
  const uint32_t effectiveRetryIntervalMs = retryIntervalMs > 0 ? retryIntervalMs : 500;
  const int64_t totalMs = static_cast<int64_t>(effectiveTimeoutMs) * (retryTimes + 1) +
                          static_cast<int64_t>(effectiveRetryIntervalMs) * retryTimes + 1000;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(totalMs));
  auto status = stub->RequestAndWait(&ctx, req, &resp);
  if (!status.ok()) {
    if (error != nullptr) {
      *error = std::format("请求响应 RPC 失败: {}", status.error_message());
    }
    LOG_ERROR("DLT645 MQTT 请求响应 RPC 失败: 连接={}, 请求主题={}, 原因={}",
              connectionLabel(), requestTopic, status.error_message());
    return status;
  }
  if (!resp.ok()) {
    if (error != nullptr) {
      *error = resp.message();
    }
    LOG_ERROR("DLT645 MQTT 请求响应失败: 连接={}, 请求主题={}, 原因={}", connectionLabel(),
              requestTopic, resp.message());
    return grpc::Status(grpc::StatusCode::INTERNAL, resp.message());
  }
  if (outResponsePayload != nullptr) {
    *outResponsePayload = resp.payload();
  }
  LOG_INFO("DLT645 MQTT 请求响应成功: 连接={}, 请求主题={}, 响应主题={}, 负载长度={}",
           connectionLabel(), requestTopic, responseTopic, resp.payload().size());
  return grpc::Status::OK;
}

std::unique_ptr<grpc::ClientReaderInterface<MQTTManagerProto::SubscribeResponse>> MqttClient::Subscribe(
    grpc::ClientContext* context, const std::vector<MQTTManagerProto::TopicFilter>& topics) {
  if (context == nullptr || topics.empty()) {
    return nullptr;
  }
  if (!hasConfig()) {
    return nullptr;
  }
  auto stub = getStub();
  MQTTManagerProto::SubscribeRequest req;
  *req.mutable_connection() = makeConnection();
  for (const auto& topic : topics) {
    *req.add_topics() = topic;
  }
  return stub->Subscribe(context, req);
}

std::string MqttClient::connectionLabel() const {
  std::lock_guard<std::mutex> lock(mu_);
  return std::format("{}:{}(client_id={})", config_.host(), config_.port(), config_.client_id());
}

MQTTManagerProto::ConnectionInfo MqttClient::makeConnection() const {
  MQTTManagerProto::ConnectionInfo info;
  std::lock_guard<std::mutex> lock(mu_);
  info.set_host(config_.host());
  info.set_port(config_.port());
  info.set_client_id(config_.client_id());
  info.set_username(config_.username());
  info.set_password(config_.password());
  info.set_keepalive_sec(config_.keepalive_sec());
  info.set_clean_session(config_.clean_session());
  info.set_connect_timeout_ms(config_.connect_timeout_ms());
  return info;
}

std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface> MqttClient::getStub() {
  std::lock_guard<std::mutex> lock(mu_);
  if (hasInjectedStub_ && injectedStub_) {
    if (stub_ != injectedStub_) {
      stub_ = injectedStub_;
    }
    return injectedStub_;
  }
  ensureStubLocked();
  return stub_;
}

void MqttClient::ensureStubLocked() {
  if (hasInjectedStub_ && injectedStub_) {
    stub_ = injectedStub_;
    return;
  }
  if (stub_) {
    return;
  }
  if (serverAddress_.empty()) {
    serverAddress_ = buildUnixSocketAddress();
  }
  channel_ = grpc::CreateChannel(serverAddress_, grpc::InsecureChannelCredentials());
  auto concrete = MQTTManagerProto::MQTTManagerService::NewStub(channel_);
  stub_ = std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface>(concrete.release());
}

std::string MqttClient::buildUnixSocketAddress() {
  std::error_code ec;
  auto dir = std::filesystem::path("./socket");
  if (!std::filesystem::exists(dir, ec)) {
    std::filesystem::create_directories(dir, ec);
  }

  auto absDir = std::filesystem::canonical(dir, ec);
  if (ec) {
    absDir = std::filesystem::absolute(dir, ec);
  }
  auto sockPath = absDir / (std::string(kDefaultModuleName) + ".sock");
  return std::format("unix:{}", sockPath.string());
}

}  // namespace DLT645
