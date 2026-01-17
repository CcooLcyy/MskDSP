#include "DataCenterCore.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace DataCenter {
grpc::Status DataCenterCore::UpsertPointTable(const DataCenterProto::UpsertPointTableRequest &request) {
  if (request.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  for (const auto &tag : request.tags()) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags 包含空字符串");
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  auto it = pointTables_.find(connId);
  if (it == pointTables_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "点表未找到");
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
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables 包含 conn_id=0");
    }
    auto &set = next[table.conn_id()];
    for (const auto &tag : table.tags()) {
      if (tag.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables 包含空 tag");
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
