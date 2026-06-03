#include "DLT645PointTableStore.h"

#include <unordered_set>
#include <utility>

#include "DLT645PointTable.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace DLT645 {
namespace {
grpc::Status validatePointTablesConfig(const DLT645Proto::PointTablesConfig &config) {
  std::unordered_set<std::string> connNames;
  for (const auto &table : config.point_tables()) {
    if (table.conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables 包含空 conn_name");
    }
    if (!connNames.emplace(table.conn_name()).second) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables 包含重复 conn_name");
    }
    PointTable checker;
    auto status = checker.Upsert(table.points(), table.blocks(), true);
    if (!status.ok()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "point_tables 包含非法点表: " + status.error_message());
    }
  }
  return grpc::Status::OK;
}
}  // namespace

DLT645PointTableStore::DLT645PointTableStore(std::filesystem::path configDbPath) :
  configDbPath_(std::move(configDbPath)) {}

grpc::Status DLT645PointTableStore::Save(const DLT645Proto::PointTablesConfig &config) {
  mskdsp::detail::ProtoSqliteStore<DLT645Proto::PointTablesConfig> store(
      configDbPath_, "DLT645", "point_tables", "DLT645Proto.PointTablesConfig", validatePointTablesConfig);
  return store.Save(config);
}

grpc::Status DLT645PointTableStore::Load(DLT645Proto::PointTablesConfig *out) {
  mskdsp::detail::ProtoSqliteStore<DLT645Proto::PointTablesConfig> store(
      configDbPath_, "DLT645", "point_tables", "DLT645Proto.PointTablesConfig", validatePointTablesConfig);
  return store.Load(out);
}

std::filesystem::path DLT645PointTableStore::databasePath() const {
  return configDbPath_;
}
}  // namespace DLT645
