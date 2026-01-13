#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "DataCenter.pb.h"

namespace DataCenter {
class DataCenterPointTableStore {
public:
  explicit DataCenterPointTableStore(std::filesystem::path pointTablesPath = std::filesystem::path("./conf/dataCenter/point_tables.pb"));

  grpc::Status Save(const DataCenterProto::PointTablesConfig& config);
  grpc::Status Load(DataCenterProto::PointTablesConfig* out);

  std::filesystem::path pointTablesPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path pointTablesPath_;
};
}  // namespace DataCenter

