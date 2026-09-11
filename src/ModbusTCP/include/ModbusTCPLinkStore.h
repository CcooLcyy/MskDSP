#pragma once
#include <filesystem>
#include <grpcpp/support/status.h>
#include "ModbusTCP.pb.h"
namespace ModbusTCP {
class LinkStore {
public:
  explicit LinkStore(std::filesystem::path path = "./conf/config.db");
  grpc::Status Save(const ModbusTCPProto::LinksConfig& config);
  grpc::Status Load(ModbusTCPProto::LinksConfig* out);
private:
  std::filesystem::path path_;
};
}
