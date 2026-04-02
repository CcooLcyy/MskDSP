#include "ModbusRTUMqttStore.h"

#include <utility>

#include "mskdsp/detail/ProtoFileStore.hpp"

namespace ModbusRTU {
namespace {
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

ModbusRTUMqttStore::ModbusRTUMqttStore(std::filesystem::path mqttPath) :
  mqttPath_(std::move(mqttPath)) {}

grpc::Status ModbusRTUMqttStore::Save(const ModbusRTUProto::MqttConfig& config) {
  mskdsp::detail::ProtoFileStore<ModbusRTUProto::MqttConfig> store(mqttPath_, validateMqttConfig);
  return store.Save(config);
}

grpc::Status ModbusRTUMqttStore::Load(ModbusRTUProto::MqttConfig* out) {
  mskdsp::detail::ProtoFileStore<ModbusRTUProto::MqttConfig> store(mqttPath_, validateMqttConfig);
  return store.Load(out);
}

std::filesystem::path ModbusRTUMqttStore::mqttPath() const {
  return mqttPath_;
}

std::filesystem::path ModbusRTUMqttStore::backupPath() const {
  mskdsp::detail::ProtoFileStore<ModbusRTUProto::MqttConfig> store(mqttPath_, validateMqttConfig);
  return store.backupPath();
}

std::filesystem::path ModbusRTUMqttStore::tmpPath() const {
  mskdsp::detail::ProtoFileStore<ModbusRTUProto::MqttConfig> store(mqttPath_, validateMqttConfig);
  return store.tmpPath();
}
}  // namespace ModbusRTU
