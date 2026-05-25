#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <grpcpp/support/status.h>

#include "DataCenter.pb.h"

namespace DataCenter {
class DataCenterRouteStore {
public:
  using TraceFn = std::function<void(const std::string&)>;

  explicit DataCenterRouteStore(std::filesystem::path routesPath = std::filesystem::path("./conf/dataCenter/routes.pb"));

  grpc::Status Save(const DataCenterProto::RoutesConfig& config, TraceFn trace = {});
  grpc::Status Load(DataCenterProto::RoutesConfig* out, TraceFn trace = {});

  std::filesystem::path routesPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path routesPath_;
};
}  // namespace DataCenter
