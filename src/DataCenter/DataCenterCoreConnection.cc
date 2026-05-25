#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DataCenterCore.h"

namespace DataCenter {
void DataCenterCore::rewriteConnectionKeyReferences(const ConnKey &oldKey, const ConnKey &newKey) {
  if (oldKey == newKey) {
    return;
  }

  std::unordered_map<StableEndpointKey, StableEndpointKeySet, StableEndpointKeyHash> rewrittenRoutes;
  for (const auto &[src, dstSet] : routes_) {
    StableEndpointKey nextSrc = src;
    if (nextSrc.moduleName == oldKey.moduleName && nextSrc.connName == oldKey.connName) {
      nextSrc.moduleName = newKey.moduleName;
      nextSrc.connName = newKey.connName;
    }
    auto &nextDstSet = rewrittenRoutes[std::move(nextSrc)];
    for (auto dst : dstSet) {
      if (dst.moduleName == oldKey.moduleName && dst.connName == oldKey.connName) {
        dst.moduleName = newKey.moduleName;
        dst.connName = newKey.connName;
      }
      nextDstSet.emplace(std::move(dst));
    }
  }
  routes_ = std::move(rewrittenRoutes);
}

grpc::Status DataCenterCore::GetConnectionByKey(const DataCenterProto::ConnectionKey &key, DataCenterProto::ConnectionInfo *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateConnKey(key);
  if (!status.ok()) {
    return status;
  }

  ConnKey lookup{key.module_name(), key.conn_name()};
  auto it = connIdsByKey_.find(lookup);
  if (it == connIdsByKey_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接未找到");
  }
  auto connIt = connections_.find(it->second);
  if (connIt == connections_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接未找到");
  }
  *out = connIt->second;
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::ReplaceConnectionsConfig(const DataCenterProto::ConnectionsConfig &config) {
  std::unordered_map<uint32_t, DataCenterProto::ConnectionInfo> nextConnections;
  std::unordered_map<ConnKey, uint32_t, ConnKeyHash> nextByKey;

  uint32_t maxId = 0;
  for (const auto &conn : config.conns()) {
    if (conn.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含 conn_id=0");
    }
    if (conn.module_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含空 module_name");
    }
    if (conn.conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含空 conn_name");
    }

    if (!nextConnections.emplace(conn.conn_id(), conn).second) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含重复的 conn_id");
    }

    ConnKey key{conn.module_name(), conn.conn_name()};
    if (!nextByKey.emplace(std::move(key), conn.conn_id()).second) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含重复的 (module_name, conn_name)");
    }

    maxId = std::max(maxId, conn.conn_id());
  }

  uint32_t computedNext = 1;
  if (maxId == std::numeric_limits<uint32_t>::max()) {
    computedNext = 0;
  } else if (!nextConnections.empty()) {
    computedNext = maxId + 1;
  }

  uint32_t nextId = config.next_conn_id();
  if (nextId == 0) {
    nextId = computedNext;
  } else if (computedNext != 0 && nextId < computedNext) {
    nextId = computedNext;
  }

  connections_ = std::move(nextConnections);
  connIdsByKey_ = std::move(nextByKey);
  nextConnId_ = nextId;
  return grpc::Status::OK;
}

DataCenterProto::ConnectionsConfig DataCenterCore::DumpConnectionsConfig() const {
  DataCenterProto::ConnectionsConfig config;
  config.set_next_conn_id(nextConnId_);

  std::vector<uint32_t> connIds;
  connIds.reserve(connections_.size());
  for (const auto &[connId, _] : connections_) {
    connIds.emplace_back(connId);
  }
  std::sort(connIds.begin(), connIds.end());
  for (auto connId : connIds) {
    auto it = connections_.find(connId);
    if (it == connections_.end()) {
      continue;
    }
    *config.add_conns() = it->second;
  }
  return config;
}

grpc::Status DataCenterCore::GetOrCreateConnection(const DataCenterProto::GetOrCreateConnectionRequest &request, DataCenterProto::ConnectionInfo *out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateConnKey(request.key());
  if (!status.ok()) {
    return status;
  }

  ConnKey key{request.key().module_name(), request.key().conn_name()};
  auto it = connIdsByKey_.find(key);
  if (it != connIdsByKey_.end()) {
    auto connIt = connections_.find(it->second);
    if (connIt != connections_.end()) {
      *out = connIt->second;
      return grpc::Status::OK;
    }
  }

  if (nextConnId_ == 0) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "conn_id 已耗尽");
  }

  uint32_t connId = nextConnId_;
  while (connections_.contains(connId)) {
    if (connId == std::numeric_limits<uint32_t>::max()) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "conn_id 已耗尽");
    }
    ++connId;
  }
  nextConnId_ = (connId == std::numeric_limits<uint32_t>::max()) ? 0 : (connId + 1);

  DataCenterProto::ConnectionInfo conn;
  conn.set_conn_id(connId);
  conn.set_module_name(request.key().module_name());
  conn.set_conn_name(request.key().conn_name());

  connections_.emplace(connId, conn);
  connIdsByKey_.emplace(std::move(key), connId);
  *out = std::move(conn);
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::RenameConnection(const DataCenterProto::RenameConnectionRequest &request, DataCenterProto::ConnectionInfo *out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateConnKey(request.old_key());
  if (!status.ok()) {
    return status;
  }
  status = validateConnKey(request.new_key());
  if (!status.ok()) {
    return status;
  }

  ConnKey oldKey{request.old_key().module_name(), request.old_key().conn_name()};
  auto it = connIdsByKey_.find(oldKey);
  if (it == connIdsByKey_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接未找到");
  }
  const uint32_t connId = it->second;

  ConnKey newKey{request.new_key().module_name(), request.new_key().conn_name()};
  if (oldKey == newKey) {
    auto connIt = connections_.find(connId);
    if (connIt == connections_.end()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接未找到");
    }
    *out = connIt->second;
    return grpc::Status::OK;
  }

  auto newIt = connIdsByKey_.find(newKey);
  if (newIt != connIdsByKey_.end() && newIt->second != connId) {
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "连接已存在");
  }

  connIdsByKey_.erase(it);
  connIdsByKey_.emplace(newKey, connId);

  auto tagsIt = connTagsByKey_.find(oldKey);
  if (tagsIt != connTagsByKey_.end()) {
    auto tags = std::move(tagsIt->second);
    connTagsByKey_.erase(tagsIt);
    connTagsByKey_[newKey] = std::move(tags);
  }

  auto connIt = connections_.find(connId);
  if (connIt == connections_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接未找到");
  }
  rewriteConnectionKeyReferences(oldKey, newKey);
  connIt->second.set_module_name(request.new_key().module_name());
  connIt->second.set_conn_name(request.new_key().conn_name());
  *out = connIt->second;
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::DeleteConnection(const DataCenterProto::DeleteConnectionRequest &request) {
  auto status = validateConnKey(request.key());
  if (!status.ok()) {
    return status;
  }

  ConnKey key{request.key().module_name(), request.key().conn_name()};
  auto it = connIdsByKey_.find(key);
  if (it == connIdsByKey_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接未找到");
  }
  const uint32_t connId = it->second;

  connIdsByKey_.erase(it);
  connections_.erase(connId);
  connTagsByKey_.erase(key);

  for (auto routeIt = routes_.begin(); routeIt != routes_.end();) {
    if (routeIt->first.moduleName == key.moduleName && routeIt->first.connName == key.connName) {
      routeIt = routes_.erase(routeIt);
      continue;
    }

    auto &dstSet = routeIt->second;
    for (auto dstIt = dstSet.begin(); dstIt != dstSet.end();) {
      if (dstIt->moduleName == key.moduleName && dstIt->connName == key.connName) {
        dstIt = dstSet.erase(dstIt);
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

  for (auto latestIt = latestByDst_.begin(); latestIt != latestByDst_.end();) {
    const auto &update = latestIt->second;
    if (latestIt->first.connId == connId || update.src_conn_id() == connId || update.dst_conn_id() == connId) {
      latestIt = latestByDst_.erase(latestIt);
    } else {
      ++latestIt;
    }
  }

  return grpc::Status::OK;
}

grpc::Status DataCenterCore::UpsertConnection(const DataCenterProto::UpsertConnectionRequest &request) {
  const auto &conn = request.conn();
  if (conn.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  if (conn.module_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "module_name 不能为空");
  }
  if (conn.conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }

  auto existing = connections_.find(conn.conn_id());
  if (existing == connections_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "conn_id 未分配");
  }

  ConnKey nextKey{conn.module_name(), conn.conn_name()};
  auto nextIt = connIdsByKey_.find(nextKey);
  if (nextIt != connIdsByKey_.end() && nextIt->second != conn.conn_id()) {
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "连接已存在");
  }

  const auto &cur = existing->second;
  ConnKey curKey{cur.module_name(), cur.conn_name()};
  if (!cur.module_name().empty() && !cur.conn_name().empty()) {
    connIdsByKey_.erase(curKey);
  }

  connIdsByKey_[std::move(nextKey)] = conn.conn_id();
  auto tagsIt = connTagsByKey_.find(curKey);
  if (tagsIt != connTagsByKey_.end()) {
    auto tags = std::move(tagsIt->second);
    connTagsByKey_.erase(tagsIt);
    connTagsByKey_[ConnKey{conn.module_name(), conn.conn_name()}] = std::move(tags);
  }
  rewriteConnectionKeyReferences(curKey, ConnKey{conn.module_name(), conn.conn_name()});
  existing->second = conn;
  return grpc::Status::OK;
}

DataCenterProto::ListConnectionsResponse DataCenterCore::ListConnections() const {
  DataCenterProto::ListConnectionsResponse resp;
  std::vector<DataCenterProto::ConnectionInfo> tmp;
  tmp.reserve(connections_.size());
  for (const auto &[_, conn] : connections_) {
    tmp.emplace_back(conn);
  }
  std::sort(tmp.begin(), tmp.end(), [](const auto &a, const auto &b) { return a.conn_id() < b.conn_id(); });
  for (const auto &conn : tmp) {
    *resp.add_conns() = conn;
  }
  return resp;
}
}  // namespace DataCenter
