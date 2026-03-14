#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "IEC104.pb.h"

namespace IEC104 {
class IEC104PointTableStore {
public:
  explicit IEC104PointTableStore(std::filesystem::path pointTablesPath = std::filesystem::path("./conf/IEC104/point_tables.pb"));

  grpc::Status Save(const IEC104Proto::PointTablesConfig& config);
  grpc::Status Load(IEC104Proto::PointTablesConfig* out);

  std::filesystem::path pointTablesPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  static grpc::Status ValidatePointTablesConfig(const IEC104Proto::PointTablesConfig& config);

  std::filesystem::path pointTablesPath_;
};
}  // namespace IEC104
