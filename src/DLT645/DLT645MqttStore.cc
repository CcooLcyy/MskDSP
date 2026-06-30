#include "DLT645MqttStore.h"

#include <utility>

#include "Logger.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace DLT645 {
namespace {
void logConfigStoreTrace(const std::string& message) {
  LOG_INFO("{}", message);
}

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

DLT645MqttStore::DLT645MqttStore(std::filesystem::path configDbPath) :
  configDbPath_(std::move(configDbPath)) {}

grpc::Status DLT645MqttStore::Save(const DLT645Proto::MqttConfig &config) {
  mskdsp::detail::ProtoSqliteStore<DLT645Proto::MqttConfig> store(
      configDbPath_, "DLT645", "mqtt", "DLT645Proto.MqttConfig", validateMqttConfig, logConfigStoreTrace);
  return store.Save(config);
}

grpc::Status DLT645MqttStore::Load(DLT645Proto::MqttConfig *out) {
  mskdsp::detail::ProtoSqliteStore<DLT645Proto::MqttConfig> store(
      configDbPath_, "DLT645", "mqtt", "DLT645Proto.MqttConfig", validateMqttConfig, logConfigStoreTrace);
  return store.Load(out);
}

std::filesystem::path DLT645MqttStore::databasePath() const {
  return configDbPath_;
}
}  // namespace DLT645
