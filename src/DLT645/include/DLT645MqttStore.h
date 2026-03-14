#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "DLT645.pb.h"

namespace DLT645 {
class DLT645MqttStore {
public:
  explicit DLT645MqttStore(std::filesystem::path mqttPath = std::filesystem::path("./conf/DLT645/mqtt.pb"));

  grpc::Status Save(const DLT645Proto::MqttConfig &config);
  grpc::Status Load(DLT645Proto::MqttConfig *out);

  std::filesystem::path mqttPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path mqttPath_;
};
}  // namespace DLT645
