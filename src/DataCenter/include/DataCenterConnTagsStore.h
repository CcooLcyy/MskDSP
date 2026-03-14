#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "DataCenter.pb.h"

namespace DataCenter {
class DataCenterConnTagsStore {
public:
  explicit DataCenterConnTagsStore(std::filesystem::path connTagsPath = std::filesystem::path("./conf/dataCenter/conn_tags.pb"));

  grpc::Status Save(const DataCenterProto::ConnTagsConfig& config);
  grpc::Status Load(DataCenterProto::ConnTagsConfig* out);

  std::filesystem::path connTagsPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path connTagsPath_;
};
}  // namespace DataCenter
