#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "DataCenter.pb.h"

namespace DataCenter {
class DataCenterConnectionStore {
public:
  explicit DataCenterConnectionStore(std::filesystem::path connectionsPath = std::filesystem::path("./conf/dataCenter/connections.pb"));

  grpc::Status Save(const DataCenterProto::ConnectionsConfig& config);
  grpc::Status Load(DataCenterProto::ConnectionsConfig* out);

  std::filesystem::path connectionsPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path connectionsPath_;
};
}  // namespace DataCenter

