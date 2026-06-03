#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "DLT645.pb.h"

namespace DLT645 {
class DLT645LinkStore {
public:
  explicit DLT645LinkStore(std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const DLT645Proto::LinksConfig &config);
  grpc::Status Load(DLT645Proto::LinksConfig *out);

  std::filesystem::path databasePath() const;

private:
  std::filesystem::path configDbPath_;
};
}  // namespace DLT645
