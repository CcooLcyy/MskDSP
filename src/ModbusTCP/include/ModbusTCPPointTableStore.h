#pragma once
#include <filesystem>
#include <grpcpp/support/status.h>
#include "ModbusTCP.pb.h"
namespace ModbusTCP {
class PointTableStore {
public:
  explicit PointTableStore(std::filesystem::path path = "./conf/config.db");
  grpc::Status Save(const ModbusTCPProto::PointTablesConfig& config);
  grpc::Status Load(ModbusTCPProto::PointTablesConfig* out);
private:
  std::filesystem::path path_;
};
}
