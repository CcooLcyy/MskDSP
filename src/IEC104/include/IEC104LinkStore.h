#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "IEC104.pb.h"

namespace IEC104 {
class IEC104LinkStore {
public:
  explicit IEC104LinkStore(std::filesystem::path linksPath = std::filesystem::path("./conf/IEC104/links.pb"));

  grpc::Status Save(const IEC104Proto::LinksConfig& config);
  grpc::Status Load(IEC104Proto::LinksConfig* out);

  std::filesystem::path linksPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  static grpc::Status ValidateLinksConfig(const IEC104Proto::LinksConfig& config);

  std::filesystem::path linksPath_;
};
}  // namespace IEC104
