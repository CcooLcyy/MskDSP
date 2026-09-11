#include "ModbusTCPPointTableStore.h"
#include <unordered_set>
#include <utility>
#include "Logger.h"
#include "ModbusRTUPointTable.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"
namespace ModbusTCP {
namespace {
grpc::Status validate(const ModbusTCPProto::PointTablesConfig& c) {
  std::unordered_set<std::string> names;
  for (const auto& table : c.point_tables()) {
    if (table.conn_name().empty() || !names.insert(table.conn_name()).second) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "ModbusTCP 持久化点表连接名非法");
    ModbusRTU::PointTable checker;
    if (auto status = checker.Upsert(table.points(), true); !status.ok()) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "ModbusTCP 持久化点表非法: " + status.error_message());
  }
  return grpc::Status::OK;
}
void trace(const std::string& message) { LOG_INFO("{}", message); }
}
PointTableStore::PointTableStore(std::filesystem::path path) : path_(std::move(path)) {}
grpc::Status PointTableStore::Save(const ModbusTCPProto::PointTablesConfig& c) { mskdsp::detail::ProtoSqliteStore<ModbusTCPProto::PointTablesConfig> s(path_, "ModbusTCP", "point_tables", "ModbusTCPProto.PointTablesConfig", validate, trace); return s.Save(c); }
grpc::Status PointTableStore::Load(ModbusTCPProto::PointTablesConfig* out) { mskdsp::detail::ProtoSqliteStore<ModbusTCPProto::PointTablesConfig> s(path_, "ModbusTCP", "point_tables", "ModbusTCPProto.PointTablesConfig", validate, trace); return s.Load(out); }
}
