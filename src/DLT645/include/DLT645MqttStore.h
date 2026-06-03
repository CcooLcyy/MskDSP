#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "DLT645.pb.h"

namespace DLT645 {
class DLT645MqttStore {
public:
  explicit DLT645MqttStore(std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status Save(const DLT645Proto::MqttConfig &config);
  grpc::Status Load(DLT645Proto::MqttConfig *out);

  std::filesystem::path databasePath() const;

private:
  std::filesystem::path configDbPath_;
};
}  // namespace DLT645
