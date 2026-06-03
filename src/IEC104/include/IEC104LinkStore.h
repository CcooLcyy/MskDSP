#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "IEC104.pb.h"

namespace IEC104 {
class IEC104LinkStore {
public:
  explicit IEC104LinkStore(std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const IEC104Proto::LinksConfig& config);
  grpc::Status Load(IEC104Proto::LinksConfig* out);

  std::filesystem::path databasePath() const;

private:
  static grpc::Status ValidateLinksConfig(const IEC104Proto::LinksConfig& config);

  std::filesystem::path configDbPath_;
};
}  // namespace IEC104
