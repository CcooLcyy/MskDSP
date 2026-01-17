#include "DataCenterPointTableStore.h"

#include "detail/ProtoFileStore.hpp"

namespace DataCenter {
namespace {
grpc::Status validatePointTablesConfig(const DataCenterProto::PointTablesConfig& config) {
  for (const auto& table : config.point_tables()) {
    if (table.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables 包含 conn_id=0");
    }
    for (const auto& tag : table.tags()) {
      if (tag.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables 包含空 tag");
      }
    }
  }
  return grpc::Status::OK;
}
}  // namespace

DataCenterPointTableStore::DataCenterPointTableStore(std::filesystem::path pointTablesPath) :
  pointTablesPath_(std::move(pointTablesPath)) {}

grpc::Status DataCenterPointTableStore::Save(const DataCenterProto::PointTablesConfig& config) {
  detail::ProtoFileStore<DataCenterProto::PointTablesConfig> store(pointTablesPath_, validatePointTablesConfig);
  return store.Save(config);
}

grpc::Status DataCenterPointTableStore::Load(DataCenterProto::PointTablesConfig* out) {
  detail::ProtoFileStore<DataCenterProto::PointTablesConfig> store(pointTablesPath_, validatePointTablesConfig);
  return store.Load(out);
}

std::filesystem::path DataCenterPointTableStore::pointTablesPath() const {
  return pointTablesPath_;
}

std::filesystem::path DataCenterPointTableStore::backupPath() const {
  detail::ProtoFileStore<DataCenterProto::PointTablesConfig> store(pointTablesPath_, validatePointTablesConfig);
  return store.backupPath();
}

std::filesystem::path DataCenterPointTableStore::tmpPath() const {
  detail::ProtoFileStore<DataCenterProto::PointTablesConfig> store(pointTablesPath_, validatePointTablesConfig);
  return store.tmpPath();
}
}  // namespace DataCenter
