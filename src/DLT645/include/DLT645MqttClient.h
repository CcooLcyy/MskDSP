#pragma once

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "DLT645.pb.h"
#include "MQTTManager.grpc.pb.h"
#include "MQTTManager.pb.h"

namespace DLT645 {

class MqttClient {
public:
  explicit MqttClient(std::string moduleName);

  void setConfig(const DLT645Proto::MqttConfig& config);
  void setStub(std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface> stub);
  bool hasConfig() const;
  bool getConfig(DLT645Proto::MqttConfig* out) const;

  grpc::Status Publish(const std::string& topic, const std::string& payload, std::string* error);
  grpc::Status RequestAndWait(const std::string& requestTopic,
                              const std::string& responseTopic,
                              const std::string& payload,
                              uint32_t timeoutMs,
                              uint32_t retryTimes,
                              uint32_t retryIntervalMs,
                              const std::string& matchField,
                              std::string* outResponsePayload,
                              std::string* error);
  std::unique_ptr<grpc::ClientReaderInterface<MQTTManagerProto::SubscribeResponse>> Subscribe(
      grpc::ClientContext* context, const std::vector<MQTTManagerProto::TopicFilter>& topics);

  std::string connectionLabel() const;

private:
  MQTTManagerProto::ConnectionInfo makeConnection() const;
  std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface> getStub();
  void ensureStubLocked();
  static std::string buildUnixSocketAddress();

  std::string moduleName_;
  mutable std::mutex mu_;
  bool hasConfig_{false};
  bool hasInjectedStub_{false};
  DLT645Proto::MqttConfig config_;
  std::string serverAddress_;
  std::shared_ptr<grpc::Channel> channel_;
  std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface> injectedStub_;
  std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface> stub_;
};

}  // namespace DLT645
