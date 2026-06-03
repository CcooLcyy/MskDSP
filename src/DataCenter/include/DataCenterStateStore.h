#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <grpcpp/support/status.h>

#include "DataCenter.pb.h"

namespace DataCenter {
class DataCenterStateStore {
public:
  using TraceFn = std::function<void(const std::string&)>;

  explicit DataCenterStateStore(std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const DataCenterProto::DataCenterState& state, TraceFn trace = {});
  grpc::Status Load(DataCenterProto::DataCenterState* out, TraceFn trace = {});

  std::filesystem::path databasePath() const;

private:
  std::filesystem::path configDbPath_;
};
}  // namespace DataCenter
