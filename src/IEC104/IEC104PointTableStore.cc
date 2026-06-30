#include "IEC104PointTableStore.h"

#include <string>
#include <unordered_set>
#include <utility>

#include "IEC104PointTable.h"
#include "Logger.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace IEC104 {
namespace {
void logConfigStoreTrace(const std::string& message) {
  LOG_INFO("{}", message);
}
}  // namespace

IEC104PointTableStore::IEC104PointTableStore(std::filesystem::path configDbPath) :
  configDbPath_(std::move(configDbPath)) {}

grpc::Status IEC104PointTableStore::Save(const IEC104Proto::PointTablesConfig& config) {
  mskdsp::detail::ProtoSqliteStore<IEC104Proto::PointTablesConfig> store(
      configDbPath_, "IEC104", "point_tables", "IEC104Proto.PointTablesConfig", ValidatePointTablesConfig, logConfigStoreTrace);
  return store.Save(config);
}

grpc::Status IEC104PointTableStore::Load(IEC104Proto::PointTablesConfig* out) {
  mskdsp::detail::ProtoSqliteStore<IEC104Proto::PointTablesConfig> store(
      configDbPath_, "IEC104", "point_tables", "IEC104Proto.PointTablesConfig", ValidatePointTablesConfig, logConfigStoreTrace);
  return store.Load(out);
}

std::filesystem::path IEC104PointTableStore::databasePath() const {
  return configDbPath_;
}

grpc::Status IEC104PointTableStore::ValidatePointTablesConfig(const IEC104Proto::PointTablesConfig& config) {
  std::unordered_set<std::string> connNames;
  for (const auto& table : config.point_tables()) {
    if (table.conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables 包含空 conn_name");
    }
    auto [_, inserted] = connNames.emplace(table.conn_name());
    if (!inserted) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables 包含重复的 conn_name");
    }
    PointTable pointTable;
    auto status = pointTable.Upsert(table.points(), true);
    if (!status.ok()) {
      return status;
    }
  }
  return grpc::Status::OK;
}

}  // namespace IEC104
