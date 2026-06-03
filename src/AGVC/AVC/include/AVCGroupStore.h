#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "AVC.pb.h"

namespace AVC {

class AVCGroupStore {
public:
  explicit AVCGroupStore(std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const AVCProto::GroupsConfig& config);
  grpc::Status Load(AVCProto::GroupsConfig* out);

  std::filesystem::path databasePath() const;

private:
  std::filesystem::path configDbPath_;
};

}  // namespace AVC
