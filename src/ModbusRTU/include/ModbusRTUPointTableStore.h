#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "ModbusRTU.pb.h"

namespace ModbusRTU {
class ModbusRTUPointTableStore {
public:
  explicit ModbusRTUPointTableStore(
      std::filesystem::path pointTablesPath = std::filesystem::path("./conf/ModbusRTU/point_tables.pb"));

  grpc::Status Save(const ModbusRTUProto::PointTablesConfig& config);
  grpc::Status Load(ModbusRTUProto::PointTablesConfig* out);

  std::filesystem::path pointTablesPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path pointTablesPath_;
};
}  // namespace ModbusRTU
