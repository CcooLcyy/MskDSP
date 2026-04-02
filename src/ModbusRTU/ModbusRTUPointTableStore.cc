#include "ModbusRTUPointTableStore.h"

#include <unordered_set>
#include <utility>

#include "ModbusRTUPointTable.h"
#include "mskdsp/detail/ProtoFileStore.hpp"

namespace ModbusRTU {
namespace {
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
    auto status = checker.Upsert(table.points(), true);
    if (!status.ok()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "point_tables 包含非法点表: " + status.error_message());
    }
  }
  return grpc::Status::OK;
}
}  // namespace

ModbusRTUPointTableStore::ModbusRTUPointTableStore(std::filesystem::path pointTablesPath) :
  pointTablesPath_(std::move(pointTablesPath)) {}

grpc::Status ModbusRTUPointTableStore::Save(const ModbusRTUProto::PointTablesConfig& config) {
  mskdsp::detail::ProtoFileStore<ModbusRTUProto::PointTablesConfig> store(pointTablesPath_, validatePointTablesConfig);
  return store.Save(config);
}

grpc::Status ModbusRTUPointTableStore::Load(ModbusRTUProto::PointTablesConfig* out) {
  mskdsp::detail::ProtoFileStore<ModbusRTUProto::PointTablesConfig> store(pointTablesPath_, validatePointTablesConfig);
  return store.Load(out);
}

std::filesystem::path ModbusRTUPointTableStore::pointTablesPath() const {
  return pointTablesPath_;
}

std::filesystem::path ModbusRTUPointTableStore::backupPath() const {
  mskdsp::detail::ProtoFileStore<ModbusRTUProto::PointTablesConfig> store(pointTablesPath_, validatePointTablesConfig);
  return store.backupPath();
}

std::filesystem::path ModbusRTUPointTableStore::tmpPath() const {
  mskdsp::detail::ProtoFileStore<ModbusRTUProto::PointTablesConfig> store(pointTablesPath_, validatePointTablesConfig);
  return store.tmpPath();
}
}  // namespace ModbusRTU
