#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "ModbusRTU.pb.h"

namespace ModbusRTU {
class ModbusRTULinkStore {
public:
  explicit ModbusRTULinkStore(std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const ModbusRTUProto::LinksConfig& config);
  grpc::Status Load(ModbusRTUProto::LinksConfig* out);

  std::filesystem::path databasePath() const;

private:
  std::filesystem::path configDbPath_;
};
}  // namespace ModbusRTU
