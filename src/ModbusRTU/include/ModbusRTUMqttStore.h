#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "ModbusRTU.pb.h"

namespace ModbusRTU {
class ModbusRTUMqttStore {
public:
  explicit ModbusRTUMqttStore(std::filesystem::path mqttPath = std::filesystem::path("./conf/ModbusRTU/mqtt.pb"));

  grpc::Status Save(const ModbusRTUProto::MqttConfig& config);
  grpc::Status Load(ModbusRTUProto::MqttConfig* out);

  std::filesystem::path mqttPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path mqttPath_;
};
}  // namespace ModbusRTU
