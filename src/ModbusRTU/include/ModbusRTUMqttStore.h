#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "ModbusRTU.pb.h"

namespace ModbusRTU {
class ModbusRTUMqttStore {
public:
  explicit ModbusRTUMqttStore(std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const ModbusRTUProto::MqttConfig& config);
  grpc::Status Load(ModbusRTUProto::MqttConfig* out);

  std::filesystem::path databasePath() const;

private:
  std::filesystem::path configDbPath_;
};
}  // namespace ModbusRTU
