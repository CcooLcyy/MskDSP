#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "AGC.pb.h"

namespace AGC {

grpc::Status ValidateControlProfilesConfig(const AGCProto::ControlProfilesConfig& config);

class AGCControlProfileStore {
public:
  explicit AGCControlProfileStore(std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const AGCProto::ControlProfilesConfig& config);
  grpc::Status Load(AGCProto::ControlProfilesConfig* out);
  const std::filesystem::path& databasePath() const;

private:
  std::filesystem::path configDbPath_;
};

}  // namespace AGC
