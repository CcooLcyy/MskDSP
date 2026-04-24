#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "AVC.pb.h"

namespace AVC {

class AVCGroupStore {
public:
  explicit AVCGroupStore(std::filesystem::path groupsPath = std::filesystem::path("./conf/AVC/groups.pb"));

  grpc::Status Save(const AVCProto::GroupsConfig& config);
  grpc::Status Load(AVCProto::GroupsConfig* out);

  std::filesystem::path groupsPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path groupsPath_;
};

}  // namespace AVC
