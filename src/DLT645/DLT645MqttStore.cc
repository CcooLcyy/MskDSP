#include "DLT645MqttStore.h"

#include <utility>

#include "detail/ProtoFileStore.hpp"

namespace DLT645 {
namespace {
grpc::Status validateMqttConfig(const DLT645Proto::MqttConfig &config) {
  if (config.host().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "MQTT host 不能为空");
  }
  if (config.port() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "MQTT port 不能为空");
  }
  if (config.client_id().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "MQTT client_id 不能为空");
  }
  return grpc::Status::OK;
}
}  // namespace

DLT645MqttStore::DLT645MqttStore(std::filesystem::path mqttPath) :
  mqttPath_(std::move(mqttPath)) {}

grpc::Status DLT645MqttStore::Save(const DLT645Proto::MqttConfig &config) {
  detail::ProtoFileStore<DLT645Proto::MqttConfig> store(mqttPath_, validateMqttConfig);
  return store.Save(config);
}

grpc::Status DLT645MqttStore::Load(DLT645Proto::MqttConfig *out) {
  detail::ProtoFileStore<DLT645Proto::MqttConfig> store(mqttPath_, validateMqttConfig);
  return store.Load(out);
}

std::filesystem::path DLT645MqttStore::mqttPath() const {
  return mqttPath_;
}

std::filesystem::path DLT645MqttStore::backupPath() const {
  detail::ProtoFileStore<DLT645Proto::MqttConfig> store(mqttPath_, validateMqttConfig);
  return store.backupPath();
}

std::filesystem::path DLT645MqttStore::tmpPath() const {
  detail::ProtoFileStore<DLT645Proto::MqttConfig> store(mqttPath_, validateMqttConfig);
  return store.tmpPath();
}
}  // namespace DLT645
