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

  explicit DataCenterStateStore(std::filesystem::path statePath = std::filesystem::path("./conf/dataCenter/state.pb"));

  grpc::Status Save(const DataCenterProto::DataCenterState& state, TraceFn trace = {});
  grpc::Status Load(DataCenterProto::DataCenterState* out, TraceFn trace = {});

  std::filesystem::path statePath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path statePath_;
};
}  // namespace DataCenter
