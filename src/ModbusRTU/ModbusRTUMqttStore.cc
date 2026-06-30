#include "ModbusRTUMqttStore.h"

#include <utility>

#include "Logger.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace ModbusRTU {
namespace {
void logConfigStoreTrace(const std::string& message) {
  LOG_INFO("{}", message);
}

grpc::Status validateMqttConfig(const ModbusRTUProto::MqttConfig& config) {
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

ModbusRTUMqttStore::ModbusRTUMqttStore(std::filesystem::path configDbPath) :
  configDbPath_(std::move(configDbPath)) {}

grpc::Status ModbusRTUMqttStore::Save(const ModbusRTUProto::MqttConfig& config) {
  mskdsp::detail::ProtoSqliteStore<ModbusRTUProto::MqttConfig> store(
      configDbPath_, "ModbusRTU", "mqtt", "ModbusRTUProto.MqttConfig", validateMqttConfig, logConfigStoreTrace);
  return store.Save(config);
}

grpc::Status ModbusRTUMqttStore::Load(ModbusRTUProto::MqttConfig* out) {
  mskdsp::detail::ProtoSqliteStore<ModbusRTUProto::MqttConfig> store(
      configDbPath_, "ModbusRTU", "mqtt", "ModbusRTUProto.MqttConfig", validateMqttConfig, logConfigStoreTrace);
  return store.Load(out);
}

std::filesystem::path ModbusRTUMqttStore::databasePath() const {
  return configDbPath_;
}
}  // namespace ModbusRTU
