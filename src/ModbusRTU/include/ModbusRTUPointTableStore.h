#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "ModbusRTU.pb.h"

namespace ModbusRTU {
class ModbusRTUPointTableStore {
public:
  explicit ModbusRTUPointTableStore(std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const ModbusRTUProto::PointTablesConfig& config);
  grpc::Status Load(ModbusRTUProto::PointTablesConfig* out);

  std::filesystem::path databasePath() const;

private:
  std::filesystem::path configDbPath_;
};
}  // namespace ModbusRTU
