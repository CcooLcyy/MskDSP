#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "AGC.pb.h"

namespace AGC {

class AGCGroupStore {
public:
  explicit AGCGroupStore(std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const AGCProto::GroupsConfig& config);
  grpc::Status Load(AGCProto::GroupsConfig* out);

  std::filesystem::path databasePath() const;

private:
  std::filesystem::path configDbPath_;
};

}  // namespace AGC
