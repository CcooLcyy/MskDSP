#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "IEC61850.pb.h"

namespace IEC61850 {

class ConfigStore {
public:
  explicit ConfigStore(
      std::filesystem::path databasePath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const IEC61850Proto::PersistedConfig& config) const;
  grpc::Status Load(IEC61850Proto::PersistedConfig* out) const;
  grpc::Status Clear() const;

  const std::filesystem::path& databasePath() const;

private:
  std::filesystem::path databasePath_;
};

}  // namespace IEC61850
