#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "DataCenter.pb.h"

namespace DataCenter {
class DataCenterRouteStore {
public:
  explicit DataCenterRouteStore(std::filesystem::path routesPath = std::filesystem::path("./conf/dataCenter/routes.pb"));

  grpc::Status Save(const DataCenterProto::RoutesConfig& config);
  grpc::Status Load(DataCenterProto::RoutesConfig* out);

  std::filesystem::path routesPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path routesPath_;
};
}  // namespace DataCenter

