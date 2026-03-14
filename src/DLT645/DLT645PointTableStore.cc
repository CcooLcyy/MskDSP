#include "DLT645PointTableStore.h"

#include <unordered_set>
#include <utility>

#include "DLT645PointTable.h"
#include "detail/ProtoFileStore.hpp"

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

DLT645PointTableStore::DLT645PointTableStore(std::filesystem::path pointTablesPath) :
  pointTablesPath_(std::move(pointTablesPath)) {}

grpc::Status DLT645PointTableStore::Save(const DLT645Proto::PointTablesConfig &config) {
  detail::ProtoFileStore<DLT645Proto::PointTablesConfig> store(pointTablesPath_, validatePointTablesConfig);
  return store.Save(config);
}

grpc::Status DLT645PointTableStore::Load(DLT645Proto::PointTablesConfig *out) {
  detail::ProtoFileStore<DLT645Proto::PointTablesConfig> store(pointTablesPath_, validatePointTablesConfig);
  return store.Load(out);
}

std::filesystem::path DLT645PointTableStore::pointTablesPath() const {
  return pointTablesPath_;
}

std::filesystem::path DLT645PointTableStore::backupPath() const {
  detail::ProtoFileStore<DLT645Proto::PointTablesConfig> store(pointTablesPath_, validatePointTablesConfig);
  return store.backupPath();
}

std::filesystem::path DLT645PointTableStore::tmpPath() const {
  detail::ProtoFileStore<DLT645Proto::PointTablesConfig> store(pointTablesPath_, validatePointTablesConfig);
  return store.tmpPath();
}
}  // namespace DLT645
