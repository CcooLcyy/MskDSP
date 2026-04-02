#include "IEC104PointTableStore.h"

#include <string>
#include <unordered_set>
#include <utility>

#include "IEC104PointTable.h"
#include "mskdsp/detail/ProtoFileStore.hpp"

namespace IEC104 {

IEC104PointTableStore::IEC104PointTableStore(std::filesystem::path pointTablesPath) :
  pointTablesPath_(std::move(pointTablesPath)) {}

grpc::Status IEC104PointTableStore::Save(const IEC104Proto::PointTablesConfig& config) {
  mskdsp::detail::ProtoFileStore<IEC104Proto::PointTablesConfig> store(pointTablesPath_, ValidatePointTablesConfig);
  return store.Save(config);
}

grpc::Status IEC104PointTableStore::Load(IEC104Proto::PointTablesConfig* out) {
  mskdsp::detail::ProtoFileStore<IEC104Proto::PointTablesConfig> store(pointTablesPath_, ValidatePointTablesConfig);
  return store.Load(out);
}

std::filesystem::path IEC104PointTableStore::pointTablesPath() const {
  return pointTablesPath_;
}

std::filesystem::path IEC104PointTableStore::backupPath() const {
  mskdsp::detail::ProtoFileStore<IEC104Proto::PointTablesConfig> store(pointTablesPath_, ValidatePointTablesConfig);
  return store.backupPath();
}

std::filesystem::path IEC104PointTableStore::tmpPath() const {
  mskdsp::detail::ProtoFileStore<IEC104Proto::PointTablesConfig> store(pointTablesPath_, ValidatePointTablesConfig);
  return store.tmpPath();
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
