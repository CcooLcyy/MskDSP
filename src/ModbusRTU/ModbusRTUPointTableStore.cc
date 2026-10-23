#include "ModbusRTUPointTableStore.h"

#include <unordered_set>
#include <utility>

#include "Logger.h"
#include "ModbusRTUPointTable.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"
#include "mskdsp/ModbusTemplateBindings.hpp"

namespace ModbusRTU {
namespace {
void logConfigStoreTrace(const std::string& message) {
  LOG_INFO("{}", message);
}

grpc::Status validatePointTablesConfig(const ModbusRTUProto::PointTablesConfig& config) {
  std::unordered_set<std::string> connNames;
  for (const auto& table : config.point_tables()) {
    if (table.conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables 包含空 conn_name");
    }
    if (!connNames.emplace(table.conn_name()).second) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables 包含重复 conn_name");
    }
    PointTable checker;
    if (auto status = mskdsp::ValidateTemplateBindings(table.template_bindings_json()); !status.ok()) return status;
    auto status = checker.Upsert(table.points(), true);
    if (!status.ok()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "point_tables 包含非法点表: " + status.error_message());
    }
  }
  return grpc::Status::OK;
}
}  // namespace

ModbusRTUPointTableStore::ModbusRTUPointTableStore(std::filesystem::path configDbPath) :
  configDbPath_(std::move(configDbPath)) {}

grpc::Status ModbusRTUPointTableStore::Save(const ModbusRTUProto::PointTablesConfig& config) {
  mskdsp::detail::ProtoSqliteStore<ModbusRTUProto::PointTablesConfig> store(
      configDbPath_, "ModbusRTU", "point_tables", "ModbusRTUProto.PointTablesConfig", validatePointTablesConfig, logConfigStoreTrace);
  return store.Save(config);
}

grpc::Status ModbusRTUPointTableStore::Load(ModbusRTUProto::PointTablesConfig* out) {
  mskdsp::detail::ProtoSqliteStore<ModbusRTUProto::PointTablesConfig> store(
      configDbPath_, "ModbusRTU", "point_tables", "ModbusRTUProto.PointTablesConfig", validatePointTablesConfig, logConfigStoreTrace);
  auto status = store.Load(out);
  if (status.ok()) {
    for (auto& table : *out->mutable_point_tables()) {
      if (table.revision() != 0) continue;
      table.set_revision(1);
      LOG_INFO("ModbusRTU 旧点表已迁移初始版本: conn_name={}, 版本=1", table.conn_name());
    }
  }
  return status;
}

std::filesystem::path ModbusRTUPointTableStore::databasePath() const {
  return configDbPath_;
}
}  // namespace ModbusRTU
