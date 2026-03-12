#pragma once

#include <grpcpp/channel.h>
#include <grpcpp/support/status.h>

#include <memory>
#include <mutex>
#include <string>

#include "MQTTManager.grpc.pb.h"
#include "MQTTManager.pb.h"
#include "ModbusRTU.pb.h"

namespace ModbusRTU {

class MqttClient {
public:
  explicit MqttClient(std::string moduleName);

  void setConfig(const ModbusRTUProto::MqttConfig& config);
  void setStub(std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface> stub);
  bool hasConfig() const;

  grpc::Status RequestAndWait(const std::string& requestTopic,
                              const std::string& responseTopic,
                              const std::string& payload,
                              uint32_t timeoutMs,
                              uint32_t retryTimes,
                              uint32_t retryIntervalMs,
                              const std::string& matchField,
                              std::string* outResponsePayload,
                              std::string* error);

  std::string connectionLabel() const;

private:
  MQTTManagerProto::ConnectionInfo makeConnection() const;
  std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface> getStub();
  void ensureStubLocked();
  static std::string buildUnixSocketAddress();

  std::string moduleName_;
  mutable std::mutex mu_;
  bool hasConfig_{false};
  ModbusRTUProto::MqttConfig config_;
  std::string serverAddress_;
  std::shared_ptr<grpc::Channel> channel_;
  std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface> stub_;
};

}  // namespace ModbusRTU
