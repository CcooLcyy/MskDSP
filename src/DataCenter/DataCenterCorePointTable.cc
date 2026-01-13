#include "DataCenterCore.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace DataCenter {
grpc::Status DataCenterCore::UpsertPointTable(const DataCenterProto::UpsertPointTableRequest &request) {
  if (request.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id is required");
  }
  for (const auto &tag : request.tags()) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags contains empty string");
    }
  }

  auto &table = pointTables_[request.conn_id()];
  if (request.replace()) {
    table.clear();
  }
  for (const auto &tag : request.tags()) {
    table.emplace(tag);
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::GetPointTable(uint32_t connId, DataCenterProto::PointTable *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id is required");
  }
  auto it = pointTables_.find(connId);
  if (it == pointTables_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "point table not found");
  }

  out->Clear();
  out->set_conn_id(connId);
  std::vector<std::string> tags(it->second.begin(), it->second.end());
  std::sort(tags.begin(), tags.end());
  for (const auto &tag : tags) {
    out->add_tags(tag);
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::ReplacePointTablesConfig(const DataCenterProto::PointTablesConfig &config) {
  std::unordered_map<uint32_t, std::unordered_set<std::string>> next;
  for (const auto &table : config.point_tables()) {
    if (table.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables contains conn_id=0");
    }
    auto &set = next[table.conn_id()];
    for (const auto &tag : table.tags()) {
      if (tag.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables contains empty tag");
      }
      set.emplace(tag);
    }
  }
  pointTables_ = std::move(next);
  return grpc::Status::OK;
}

DataCenterProto::PointTablesConfig DataCenterCore::DumpPointTablesConfig() const {
  DataCenterProto::PointTablesConfig config;
  std::vector<uint32_t> connIds;
  connIds.reserve(pointTables_.size());
  for (const auto &[connId, _] : pointTables_) {
    connIds.emplace_back(connId);
  }
  std::sort(connIds.begin(), connIds.end());

  for (auto connId : connIds) {
    auto it = pointTables_.find(connId);
    if (it == pointTables_.end()) {
      continue;
    }
    auto *table = config.add_point_tables();
    table->set_conn_id(connId);
    std::vector<std::string> tags(it->second.begin(), it->second.end());
    std::sort(tags.begin(), tags.end());
    for (const auto &tag : tags) {
      table->add_tags(tag);
    }
  }
  return config;
}
}  // namespace DataCenter

