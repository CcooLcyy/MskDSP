#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "ModbusRTU.pb.h"

namespace ModbusRTU {
class ModbusRTULinkStore {
public:
  explicit ModbusRTULinkStore(std::filesystem::path linksPath = std::filesystem::path("./conf/ModbusRTU/links.pb"));

  grpc::Status Save(const ModbusRTUProto::LinksConfig& config);
  grpc::Status Load(ModbusRTUProto::LinksConfig* out);

  std::filesystem::path linksPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path linksPath_;
};
}  // namespace ModbusRTU
