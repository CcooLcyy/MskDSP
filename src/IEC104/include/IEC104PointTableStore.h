#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "IEC104.pb.h"

namespace IEC104 {
class IEC104PointTableStore {
public:
  explicit IEC104PointTableStore(std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const IEC104Proto::PointTablesConfig& config);
  grpc::Status Load(IEC104Proto::PointTablesConfig* out);

  std::filesystem::path databasePath() const;

private:
  static grpc::Status ValidatePointTablesConfig(const IEC104Proto::PointTablesConfig& config);

  std::filesystem::path configDbPath_;
};
}  // namespace IEC104
