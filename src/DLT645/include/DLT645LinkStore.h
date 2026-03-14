#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "DLT645.pb.h"

namespace DLT645 {
class DLT645LinkStore {
public:
  explicit DLT645LinkStore(std::filesystem::path linksPath = std::filesystem::path("./conf/DLT645/links.pb"));

  grpc::Status Save(const DLT645Proto::LinksConfig &config);
  grpc::Status Load(DLT645Proto::LinksConfig *out);

  std::filesystem::path linksPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path linksPath_;
};
}  // namespace DLT645
