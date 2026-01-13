#include "DataCenterCore.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <tuple>
#include <utility>

namespace DataCenter {
bool DataCenterCore::EndpointKey::operator==(const EndpointKey& other) const {
  return connId == other.connId && tag == other.tag;
}

size_t DataCenterCore::EndpointKeyHash::operator()(const EndpointKey& key) const noexcept {
  size_t h1 = std::hash<uint32_t>{}(key.connId);
  size_t h2 = std::hash<std::string>{}(key.tag);
  return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
}

grpc::Status DataCenterCore::validateEndpoint(uint32_t connId, const std::string& tag) {
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id is required");
  }
  if (tag.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag is required");
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::validateEndpointAgainstPointTable(uint32_t connId, const std::string& tag) const {
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

grpc::Status DataCenterCore::UpsertConnection(const DataCenterProto::UpsertConnectionRequest& request) {
  const auto& conn = request.conn();
  if (conn.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id is required");
  }
  connections_[conn.conn_id()] = conn;
  return grpc::Status::OK;
}

DataCenterProto::ListConnectionsResponse DataCenterCore::ListConnections() const {
  DataCenterProto::ListConnectionsResponse resp;
  std::vector<DataCenterProto::ConnectionInfo> tmp;
  tmp.reserve(connections_.size());
  for (const auto& [_, conn] : connections_) {
    tmp.emplace_back(conn);
  }
  std::sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) { return a.conn_id() < b.conn_id(); });
  for (const auto& conn : tmp) {
    *resp.add_conns() = conn;
  }
  return resp;
}

grpc::Status DataCenterCore::UpsertPointTable(const DataCenterProto::UpsertPointTableRequest& request) {
  if (request.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id is required");
  }
  for (const auto& tag : request.tags()) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags contains empty string");
    }
  }

  auto& table = pointTables_[request.conn_id()];
  if (request.replace()) {
    table.clear();
  }
  for (const auto& tag : request.tags()) {
    table.emplace(tag);
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::GetPointTable(uint32_t connId, DataCenterProto::PointTable* out) const {
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
  for (const auto& tag : tags) {
    out->add_tags(tag);
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::ReplacePointTablesConfig(const DataCenterProto::PointTablesConfig& config) {
  std::unordered_map<uint32_t, std::unordered_set<std::string>> next;
  for (const auto& table : config.point_tables()) {
    if (table.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_tables contains conn_id=0");
    }
    auto& set = next[table.conn_id()];
    for (const auto& tag : table.tags()) {
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
  for (const auto& [connId, _] : pointTables_) {
    connIds.emplace_back(connId);
  }
  std::sort(connIds.begin(), connIds.end());

  for (auto connId : connIds) {
    auto it = pointTables_.find(connId);
    if (it == pointTables_.end()) {
      continue;
    }
    auto* table = config.add_point_tables();
    table->set_conn_id(connId);
    std::vector<std::string> tags(it->second.begin(), it->second.end());
    std::sort(tags.begin(), tags.end());
    for (const auto& tag : tags) {
      table->add_tags(tag);
    }
  }
  return config;
}

grpc::Status DataCenterCore::UpsertRoutes(const DataCenterProto::UpsertRoutesRequest& request) {
  if (request.replace()) {
    routes_.clear();
  }

  for (const auto& route : request.routes()) {
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

grpc::Status DataCenterCore::DeleteRoutes(const DataCenterProto::DeleteRoutesRequest& request) {
  for (const auto& route : request.routes()) {
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

DataCenterProto::ListRoutesResponse DataCenterCore::ListRoutes(const DataCenterProto::ListRoutesRequest& request) const {
  std::vector<DataCenterProto::Route> tmp;
  for (const auto& [src, dstSet] : routes_) {
    if (request.src_conn_id() != 0 && request.src_conn_id() != src.connId) {
      continue;
    }
    if (!request.src_tag().empty() && request.src_tag() != src.tag) {
      continue;
    }
    for (const auto& dst : dstSet) {
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

  std::sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) {
    return std::make_tuple(a.src().conn_id(), a.src().tag(), a.dst().conn_id(), a.dst().tag()) <
           std::make_tuple(b.src().conn_id(), b.src().tag(), b.dst().conn_id(), b.dst().tag());
  });

  DataCenterProto::ListRoutesResponse resp;
  for (const auto& route : tmp) {
    *resp.add_routes() = route;
  }
  return resp;
}

grpc::Status DataCenterCore::ReplaceRoutesConfig(const DataCenterProto::RoutesConfig& config) {
  std::unordered_map<EndpointKey, EndpointKeySet, EndpointKeyHash> next;
  for (const auto& route : config.routes()) {
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
  for (const auto& route : resp.routes()) {
    *config.add_routes() = route;
  }
  return config;
}

grpc::Status DataCenterCore::Publish(const DataCenterProto::PublishRequest& request, std::vector<DataCenterProto::PointUpdate>* outUpdates) {
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
  for (const auto& dst : it->second) {
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

grpc::Status DataCenterCore::BatchPublish(const DataCenterProto::BatchPublishRequest& request, std::vector<DataCenterProto::PointUpdate>* outUpdates) {
  if (outUpdates == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "outUpdates is null");
  }
  outUpdates->clear();
  for (const auto& point : request.points()) {
    std::vector<DataCenterProto::PointUpdate> updates;
    auto status = Publish(point, &updates);
    if (!status.ok()) {
      return status;
    }
    outUpdates->insert(outUpdates->end(), std::make_move_iterator(updates.begin()), std::make_move_iterator(updates.end()));
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::GetLatest(const DataCenterProto::GetLatestRequest& request, DataCenterProto::GetLatestResponse* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  if (request.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id is required");
  }
  for (const auto& tag : request.tags()) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags contains empty string");
    }
  }

  std::unordered_set<std::string> filter;
  if (request.tags_size() > 0) {
    filter.reserve(static_cast<size_t>(request.tags_size()));
    for (const auto& tag : request.tags()) {
      filter.emplace(tag);
    }
  }

  std::vector<DataCenterProto::PointUpdate> tmp;
  for (const auto& [dst, update] : latestByDst_) {
    if (dst.connId != request.conn_id()) {
      continue;
    }
    if (!filter.empty() && !filter.contains(dst.tag)) {
      continue;
    }
    tmp.emplace_back(update);
  }

  std::sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) { return a.dst_tag() < b.dst_tag(); });

  out->Clear();
  for (const auto& update : tmp) {
    *out->add_updates() = update;
  }
  return grpc::Status::OK;
}
}  // namespace DataCenter
