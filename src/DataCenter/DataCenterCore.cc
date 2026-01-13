#include "DataCenterCore.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <tuple>
#include <utility>

namespace DataCenter {
bool DataCenterCore::ConnKey::operator==(const ConnKey &other) const {
  return moduleName == other.moduleName && connName == other.connName;
}

size_t DataCenterCore::ConnKeyHash::operator()(const ConnKey &key) const noexcept {
  size_t h1 = std::hash<std::string>{}(key.moduleName);
  size_t h2 = std::hash<std::string>{}(key.connName);
  return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
}

bool DataCenterCore::EndpointKey::operator==(const EndpointKey &other) const {
  return connId == other.connId && tag == other.tag;
}

size_t DataCenterCore::EndpointKeyHash::operator()(const EndpointKey &key) const noexcept {
  size_t h1 = std::hash<uint32_t>{}(key.connId);
  size_t h2 = std::hash<std::string>{}(key.tag);
  return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
}

grpc::Status DataCenterCore::validateConnKey(const DataCenterProto::ConnectionKey &key) {
  if (key.module_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "module_name is required");
  }
  if (key.conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name is required");
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::validateEndpoint(uint32_t connId, const std::string &tag) {
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id is required");
  }
  if (tag.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag is required");
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::validateEndpointAgainstPointTable(uint32_t connId, const std::string &tag) const {
  auto it = pointTables_.find(connId);
  if (it == pointTables_.end()) {
    return grpc::Status::OK;
  }
  if (!it->second.contains(tag)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag not found in point table");
  }
  return grpc::Status::OK;
}

int64_t DataCenterCore::nowMs() {
  auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
  return now.time_since_epoch().count();
}

grpc::Status DataCenterCore::GetConnectionByKey(const DataCenterProto::ConnectionKey &key, DataCenterProto::ConnectionInfo *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  auto status = validateConnKey(key);
  if (!status.ok()) {
    return status;
  }

  ConnKey lookup{key.module_name(), key.conn_name()};
  auto it = connIdsByKey_.find(lookup);
  if (it == connIdsByKey_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "connection not found");
  }
  auto connIt = connections_.find(it->second);
  if (connIt == connections_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "connection not found");
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
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns contains conn_id=0");
    }
    if (conn.module_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns contains empty module_name");
    }
    if (conn.conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns contains empty conn_name");
    }

    if (!nextConnections.emplace(conn.conn_id(), conn).second) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns contains duplicate conn_id");
    }

    ConnKey key{conn.module_name(), conn.conn_name()};
    if (!nextByKey.emplace(std::move(key), conn.conn_id()).second) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns contains duplicate (module_name, conn_name)");
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
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
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "conn_id exhausted");
  }

  uint32_t connId = nextConnId_;
  while (connections_.contains(connId)) {
    if (connId == std::numeric_limits<uint32_t>::max()) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "conn_id exhausted");
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
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
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "connection not found");
  }
  const uint32_t connId = it->second;

  ConnKey newKey{request.new_key().module_name(), request.new_key().conn_name()};
  if (oldKey == newKey) {
    auto connIt = connections_.find(connId);
    if (connIt == connections_.end()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "connection not found");
    }
    *out = connIt->second;
    return grpc::Status::OK;
  }

  auto newIt = connIdsByKey_.find(newKey);
  if (newIt != connIdsByKey_.end() && newIt->second != connId) {
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "connection already exists");
  }

  connIdsByKey_.erase(it);
  connIdsByKey_.emplace(std::move(newKey), connId);

  auto connIt = connections_.find(connId);
  if (connIt == connections_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "connection not found");
  }
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
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "connection not found");
  }
  const uint32_t connId = it->second;

  connIdsByKey_.erase(it);
  connections_.erase(connId);
  pointTables_.erase(connId);

  for (auto routeIt = routes_.begin(); routeIt != routes_.end();) {
    if (routeIt->first.connId == connId) {
      routeIt = routes_.erase(routeIt);
      continue;
    }

    auto &dstSet = routeIt->second;
    for (auto dstIt = dstSet.begin(); dstIt != dstSet.end();) {
      if (dstIt->connId == connId) {
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id is required");
  }
  if (conn.module_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "module_name is required");
  }
  if (conn.conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name is required");
  }

  auto existing = connections_.find(conn.conn_id());
  if (existing == connections_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "conn_id not allocated");
  }

  ConnKey nextKey{conn.module_name(), conn.conn_name()};
  auto nextIt = connIdsByKey_.find(nextKey);
  if (nextIt != connIdsByKey_.end() && nextIt->second != conn.conn_id()) {
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "connection already exists");
  }

  const auto &cur = existing->second;
  if (!cur.module_name().empty() && !cur.conn_name().empty()) {
    ConnKey curKey{cur.module_name(), cur.conn_name()};
    connIdsByKey_.erase(curKey);
  }

  connIdsByKey_[std::move(nextKey)] = conn.conn_id();
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

grpc::Status DataCenterCore::UpsertRoutes(const DataCenterProto::UpsertRoutesRequest &request) {
  if (request.replace()) {
    routes_.clear();
  }

  for (const auto &route : request.routes()) {
    auto status = validateEndpoint(route.src().conn_id(), route.src().tag());
    if (!status.ok()) {
      return status;
    }
    status = validateEndpoint(route.dst().conn_id(), route.dst().tag());
    if (!status.ok()) {
      return status;
    }
    status = validateEndpointAgainstPointTable(route.src().conn_id(), route.src().tag());
    if (!status.ok()) {
      return status;
    }
    status = validateEndpointAgainstPointTable(route.dst().conn_id(), route.dst().tag());
    if (!status.ok()) {
      return status;
    }

    EndpointKey src{route.src().conn_id(), route.src().tag()};
    EndpointKey dst{route.dst().conn_id(), route.dst().tag()};
    routes_[std::move(src)].emplace(std::move(dst));
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::DeleteRoutes(const DataCenterProto::DeleteRoutesRequest &request) {
  for (const auto &route : request.routes()) {
    auto status = validateEndpoint(route.src().conn_id(), route.src().tag());
    if (!status.ok()) {
      return status;
    }
    status = validateEndpoint(route.dst().conn_id(), route.dst().tag());
    if (!status.ok()) {
      return status;
    }

    EndpointKey src{route.src().conn_id(), route.src().tag()};
    EndpointKey dst{route.dst().conn_id(), route.dst().tag()};

    auto srcIt = routes_.find(src);
    if (srcIt == routes_.end()) {
      continue;
    }
    srcIt->second.erase(dst);
    if (srcIt->second.empty()) {
      routes_.erase(srcIt);
    }
  }
  return grpc::Status::OK;
}

DataCenterProto::ListRoutesResponse DataCenterCore::ListRoutes(const DataCenterProto::ListRoutesRequest &request) const {
  std::vector<DataCenterProto::Route> tmp;
  for (const auto &[src, dstSet] : routes_) {
    if (request.src_conn_id() != 0 && request.src_conn_id() != src.connId) {
      continue;
    }
    if (!request.src_tag().empty() && request.src_tag() != src.tag) {
      continue;
    }
    for (const auto &dst : dstSet) {
      if (request.dst_conn_id() != 0 && request.dst_conn_id() != dst.connId) {
        continue;
      }
      if (!request.dst_tag().empty() && request.dst_tag() != dst.tag) {
        continue;
      }
      DataCenterProto::Route route;
      route.mutable_src()->set_conn_id(src.connId);
      route.mutable_src()->set_tag(src.tag);
      route.mutable_dst()->set_conn_id(dst.connId);
      route.mutable_dst()->set_tag(dst.tag);
      tmp.emplace_back(std::move(route));
    }
  }

  std::sort(tmp.begin(), tmp.end(), [](const auto &a, const auto &b) {
    return std::make_tuple(a.src().conn_id(), a.src().tag(), a.dst().conn_id(), a.dst().tag()) <
        std::make_tuple(b.src().conn_id(), b.src().tag(), b.dst().conn_id(), b.dst().tag());
  });

  DataCenterProto::ListRoutesResponse resp;
  for (const auto &route : tmp) {
    *resp.add_routes() = route;
  }
  return resp;
}

grpc::Status DataCenterCore::ReplaceRoutesConfig(const DataCenterProto::RoutesConfig &config) {
  std::unordered_map<EndpointKey, EndpointKeySet, EndpointKeyHash> next;
  for (const auto &route : config.routes()) {
    auto status = validateEndpoint(route.src().conn_id(), route.src().tag());
    if (!status.ok()) {
      return status;
    }
    status = validateEndpoint(route.dst().conn_id(), route.dst().tag());
    if (!status.ok()) {
      return status;
    }
    status = validateEndpointAgainstPointTable(route.src().conn_id(), route.src().tag());
    if (!status.ok()) {
      return status;
    }
    status = validateEndpointAgainstPointTable(route.dst().conn_id(), route.dst().tag());
    if (!status.ok()) {
      return status;
    }

    EndpointKey src{route.src().conn_id(), route.src().tag()};
    EndpointKey dst{route.dst().conn_id(), route.dst().tag()};
    next[std::move(src)].emplace(std::move(dst));
  }

  routes_ = std::move(next);
  return grpc::Status::OK;
}

DataCenterProto::RoutesConfig DataCenterCore::DumpRoutesConfig() const {
  DataCenterProto::RoutesConfig config;
  DataCenterProto::ListRoutesRequest request;
  const auto resp = ListRoutes(request);
  for (const auto &route : resp.routes()) {
    *config.add_routes() = route;
  }
  return config;
}

grpc::Status DataCenterCore::Publish(const DataCenterProto::PublishRequest &request, std::vector<DataCenterProto::PointUpdate> *outUpdates) {
  if (outUpdates == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "outUpdates is null");
  }
  auto status = validateEndpoint(request.conn_id(), request.tag());
  if (!status.ok()) {
    return status;
  }
  if (request.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "value is required");
  }

  int64_t tsMs = request.ts_ms();
  if (tsMs <= 0) {
    tsMs = nowMs();
  }

  outUpdates->clear();
  EndpointKey src{request.conn_id(), request.tag()};
  auto it = routes_.find(src);
  if (it == routes_.end()) {
    return grpc::Status::OK;
  }

  outUpdates->reserve(it->second.size());
  for (const auto &dst : it->second) {
    DataCenterProto::PointUpdate update;
    update.set_src_conn_id(request.conn_id());
    update.set_src_tag(request.tag());
    update.set_dst_conn_id(dst.connId);
    update.set_dst_tag(dst.tag);
    update.mutable_value()->CopyFrom(request.value());
    update.set_ts_ms(tsMs);
    update.set_quality(request.quality());

    latestByDst_[dst] = update;
    outUpdates->emplace_back(std::move(update));
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::BatchPublish(const DataCenterProto::BatchPublishRequest &request, std::vector<DataCenterProto::PointUpdate> *outUpdates) {
  if (outUpdates == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "outUpdates is null");
  }
  outUpdates->clear();

  for (const auto &point : request.points()) {
    auto status = validateEndpoint(point.conn_id(), point.tag());
    if (!status.ok()) {
      return status;
    }
    if (point.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "value is required");
    }
  }

  size_t estimatedUpdates = 0;
  for (const auto &point : request.points()) {
    EndpointKey src{point.conn_id(), point.tag()};
    auto it = routes_.find(src);
    if (it == routes_.end()) {
      continue;
    }
    estimatedUpdates += it->second.size();
  }
  outUpdates->reserve(estimatedUpdates);

  for (const auto &point : request.points()) {
    int64_t tsMs = point.ts_ms();
    if (tsMs <= 0) {
      tsMs = nowMs();
    }

    EndpointKey src{point.conn_id(), point.tag()};
    auto it = routes_.find(src);
    if (it == routes_.end()) {
      continue;
    }

    for (const auto &dst : it->second) {
      DataCenterProto::PointUpdate update;
      update.set_src_conn_id(point.conn_id());
      update.set_src_tag(point.tag());
      update.set_dst_conn_id(dst.connId);
      update.set_dst_tag(dst.tag);
      update.mutable_value()->CopyFrom(point.value());
      update.set_ts_ms(tsMs);
      update.set_quality(point.quality());
      outUpdates->emplace_back(std::move(update));
    }
  }

  for (const auto &update : *outUpdates) {
    EndpointKey dst{update.dst_conn_id(), update.dst_tag()};
    latestByDst_[std::move(dst)] = update;
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::GetLatest(const DataCenterProto::GetLatestRequest &request, DataCenterProto::GetLatestResponse *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  if (request.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id is required");
  }
  for (const auto &tag : request.tags()) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags contains empty string");
    }
  }

  std::unordered_set<std::string> filter;
  if (request.tags_size() > 0) {
    filter.reserve(static_cast<size_t>(request.tags_size()));
    for (const auto &tag : request.tags()) {
      filter.emplace(tag);
    }
  }

  std::vector<DataCenterProto::PointUpdate> tmp;
  for (const auto &[dst, update] : latestByDst_) {
    if (dst.connId != request.conn_id()) {
      continue;
    }
    if (!filter.empty() && !filter.contains(dst.tag)) {
      continue;
    }
    tmp.emplace_back(update);
  }

  std::sort(tmp.begin(), tmp.end(), [](const auto &a, const auto &b) { return a.dst_tag() < b.dst_tag(); });

  out->Clear();
  for (const auto &update : tmp) {
    *out->add_updates() = update;
  }
  return grpc::Status::OK;
}
}  // namespace DataCenter
