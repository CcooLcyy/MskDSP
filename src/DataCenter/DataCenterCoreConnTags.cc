#include "DataCenterCore.h"

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

#include "Logger.h"

namespace DataCenter {
size_t DataCenterCore::pruneRoutesRejectedByConnTags(const ConnKey &key, const std::unordered_set<std::string> &allowedTags) {
  size_t removed = 0;
  for (auto routeIt = routes_.begin(); routeIt != routes_.end();) {
    if (routeIt->first.moduleName == key.moduleName && routeIt->first.connName == key.connName &&
        !allowedTags.contains(routeIt->first.tag)) {
      removed += routeIt->second.size();
      routeIt = routes_.erase(routeIt);
      continue;
    }

    auto &dstSet = routeIt->second;
    for (auto dstIt = dstSet.begin(); dstIt != dstSet.end();) {
      if (dstIt->moduleName == key.moduleName && dstIt->connName == key.connName && !allowedTags.contains(dstIt->tag)) {
        dstIt = dstSet.erase(dstIt);
        ++removed;
      } else {
        ++dstIt;
      }
    }
    if (dstSet.empty()) {
      routeIt = routes_.erase(routeIt);
    } else {
      ++routeIt;
    }
  }
  return removed;
}

grpc::Status DataCenterCore::UpsertConnTags(const DataCenterProto::UpsertConnTagsRequest &request) {
  if (request.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  for (const auto &tag : request.tags()) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags 包含空字符串");
    }
  }

  ConnKey key;
  if (!tryResolveConnKey(request.conn_id(), &key)) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "conn_id 未在连接注册表中找到，无法更新连接标签注册表");
  }

  auto &table = connTagsByKey_[key];
  if (request.replace()) {
    table.clear();
  }
  for (const auto &tag : request.tags()) {
    table.emplace(tag);
  }
  if (request.replace()) {
    const auto removed = pruneRoutesRejectedByConnTags(key, table);
    if (removed > 0) {
      LOG_WARNING("DataCenter 覆盖连接标签注册表时删除不再匹配的路由: module_name={}, conn_name={}, conn_id={}, 删除路由数={}",
                  key.moduleName, key.connName, request.conn_id(), removed);
    }
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
  ConnKey key;
  if (!tryResolveConnKey(connId, &key)) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "conn_id 未在连接注册表中找到");
  }
  auto it = connTagsByKey_.find(key);
  if (it == connTagsByKey_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接标签注册表未找到");
  }

  out->Clear();
  out->set_conn_id(connId);
  out->set_module_name(key.moduleName);
  out->set_conn_name(key.connName);
  std::vector<std::string> tags(it->second.begin(), it->second.end());
  std::sort(tags.begin(), tags.end());
  for (const auto &tag : tags) {
    out->add_tags(tag);
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::ReplaceConnTagsConfig(const DataCenterProto::ConnTagsConfig &config) {
  std::unordered_map<ConnKey, std::unordered_set<std::string>, ConnKeyHash> next;
  for (const auto &table : config.conn_tags()) {
    if (table.module_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_tags 包含空 module_name");
    }
    if (table.conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_tags 包含空 conn_name");
    }
    ConnKey key{table.module_name(), table.conn_name()};
    if (!connIdsByKey_.contains(key)) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "conn_tags 引用的稳定连接主键未在连接注册表中找到");
    }
    auto &set = next[std::move(key)];
    for (const auto &tag : table.tags()) {
      if (tag.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_tags 包含空 tag");
      }
      set.emplace(tag);
    }
  }
  connTagsByKey_ = std::move(next);
  size_t removed = 0;
  for (const auto &[key, tags] : connTagsByKey_) {
    removed += pruneRoutesRejectedByConnTags(key, tags);
  }
  if (removed > 0) {
    LOG_WARNING("DataCenter 替换连接标签注册表配置时删除不再匹配的路由: 删除路由数={}", removed);
  }
  return grpc::Status::OK;
}

DataCenterProto::ConnTagsConfig DataCenterCore::DumpConnTagsConfig() const {
  DataCenterProto::ConnTagsConfig config;
  std::vector<ConnKey> keys;
  keys.reserve(connTagsByKey_.size());
  for (const auto &[key, _] : connTagsByKey_) {
    keys.emplace_back(key);
  }
  std::sort(keys.begin(), keys.end(), [](const ConnKey &a, const ConnKey &b) {
    return std::tie(a.moduleName, a.connName) < std::tie(b.moduleName, b.connName);
  });

  for (const auto &key : keys) {
    auto it = connTagsByKey_.find(key);
    if (it == connTagsByKey_.end()) {
      continue;
    }
    auto *table = config.add_conn_tags();
    table->set_module_name(key.moduleName);
    table->set_conn_name(key.connName);
    auto connIdIt = connIdsByKey_.find(key);
    if (connIdIt != connIdsByKey_.end()) {
      table->set_conn_id(connIdIt->second);
    }
    std::vector<std::string> tags(it->second.begin(), it->second.end());
    std::sort(tags.begin(), tags.end());
    for (const auto &tag : tags) {
      table->add_tags(tag);
    }
  }
  return config;
}
}  // namespace DataCenter
