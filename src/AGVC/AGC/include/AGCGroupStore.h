#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "AGC.pb.h"

namespace AGC {

class AGCGroupStore {
public:
  explicit AGCGroupStore(std::filesystem::path groupsPath = std::filesystem::path("./conf/AGC/groups.pb"));

  grpc::Status Save(const AGCProto::GroupsConfig& config);
  grpc::Status Load(AGCProto::GroupsConfig* out);

  std::filesystem::path groupsPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path groupsPath_;
};

}  // namespace AGC
