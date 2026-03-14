#include "DataCenterCore.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace DataCenter {
grpc::Status DataCenterCore::UpsertConnTags(const DataCenterProto::UpsertConnTagsRequest &request) {
  if (request.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  for (const auto &tag : request.tags()) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags 包含空字符串");
    }
  }

  auto &table = connTagsByConnId_[request.conn_id()];
  if (request.replace()) {
    table.clear();
  }
  for (const auto &tag : request.tags()) {
    table.emplace(tag);
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::GetConnTags(uint32_t connId, DataCenterProto::ConnTags *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  auto it = connTagsByConnId_.find(connId);
  if (it == connTagsByConnId_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接标签注册表未找到");
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

grpc::Status DataCenterCore::ReplaceConnTagsConfig(const DataCenterProto::ConnTagsConfig &config) {
  std::unordered_map<uint32_t, std::unordered_set<std::string>> next;
  for (const auto &table : config.conn_tags()) {
    if (table.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_tags 包含 conn_id=0");
    }
    auto &set = next[table.conn_id()];
    for (const auto &tag : table.tags()) {
      if (tag.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_tags 包含空 tag");
      }
      set.emplace(tag);
    }
  }
  connTagsByConnId_ = std::move(next);
  return grpc::Status::OK;
}

DataCenterProto::ConnTagsConfig DataCenterCore::DumpConnTagsConfig() const {
  DataCenterProto::ConnTagsConfig config;
  std::vector<uint32_t> connIds;
  connIds.reserve(connTagsByConnId_.size());
  for (const auto &[connId, _] : connTagsByConnId_) {
    connIds.emplace_back(connId);
  }
  std::sort(connIds.begin(), connIds.end());

  for (auto connId : connIds) {
    auto it = connTagsByConnId_.find(connId);
    if (it == connTagsByConnId_.end()) {
      continue;
    }
    auto *table = config.add_conn_tags();
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
