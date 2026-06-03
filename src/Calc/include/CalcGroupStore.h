#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "Calc.pb.h"

namespace Calc {

class GroupStore {
public:
  explicit GroupStore(std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const CalcProto::GroupsConfig &config);
  grpc::Status Load(CalcProto::GroupsConfig *out);

  std::filesystem::path databasePath() const;

private:
  std::filesystem::path configDbPath_;
};

}  // namespace Calc
