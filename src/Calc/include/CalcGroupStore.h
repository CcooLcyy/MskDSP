#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "Calc.pb.h"

namespace Calc {

class GroupStore {
public:
  explicit GroupStore(std::filesystem::path groupsPath = std::filesystem::path("./conf/Calc/groups.pb"));

  grpc::Status Save(const CalcProto::GroupsConfig &config);
  grpc::Status Load(CalcProto::GroupsConfig *out);

  std::filesystem::path groupsPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path groupsPath_;
};

}  // namespace Calc
