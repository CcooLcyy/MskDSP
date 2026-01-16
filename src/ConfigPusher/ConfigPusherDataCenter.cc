#include "ConfigPusherDataCenter.h"

#include <grpcpp/client_context.h>

#include <unordered_map>
#include <utility>
#include <vector>

#include "Logger.h"

namespace ConfigPusher {
namespace {
struct ConnKey {
  std::string module;
  std::string conn;

  bool operator==(const ConnKey &other) const {
    return module == other.module && conn == other.conn;
  }
};

struct ConnKeyHash {
  std::size_t operator()(const ConnKey &k) const noexcept {
    std::hash<std::string> h;
    return h(k.module) ^ (h(k.conn) << 1);
  }
};

bool HasDataCenterConfig(const ConfigPusherProto::DataCenterConfig &config) {
  if (!config.point_tables().empty()) {
    return true;
  }
  return config.has_routes() && config.routes().routes_size() > 0;
}

bool ValidateConnKey(const std::string &moduleName, const std::string &connName) {
  return !moduleName.empty() && !connName.empty();
}

bool ResolveConnId(const std::unordered_map<ConnKey, uint32_t, ConnKeyHash> &connIds,
                   const std::string &moduleName,
                   const std::string &connName,
                   uint32_t *outConnId) {
  if (outConnId == nullptr) {
    return false;
  }
  ConnKey key{moduleName, connName};
  auto it = connIds.find(key);
  if (it == connIds.end()) {
    return false;
  }
  *outConnId = it->second;
  return true;
}
}  // namespace

bool ApplyDataCenterConfig(const ConfigPusherProto::DataCenterConfig &config,
                           DataCenterProto::DataCenterService::StubInterface *stub) {
  if (stub == nullptr) {
    LOG_ERROR("DataCenter gRPC stub is null");
    return false;
  }
  if (!HasDataCenterConfig(config)) {
    LOG_INFO("DataCenter config is empty, skipping push");
    return true;
  }

  DataCenterProto::ListConnectionsResponse connections;
  grpc::ClientContext listCtx;
  DataCenterProto::Empty listReq;
  auto status = stub->ListConnections(&listCtx, listReq, &connections);
  if (!status.ok()) {
    LOG_ERROR("Failed to list DataCenter connections: {}", status.error_message());
    return false;
  }

  std::unordered_map<ConnKey, uint32_t, ConnKeyHash> connIds;
  connIds.reserve(static_cast<size_t>(connections.conns_size()));
  for (const auto &conn : connections.conns()) {
    if (conn.module_name().empty() || conn.conn_name().empty() || conn.conn_id() == 0) {
      continue;
    }
    connIds.emplace(ConnKey{conn.module_name(), conn.conn_name()}, conn.conn_id());
  }

  bool ok = true;
  struct ResolvedPointTable {
    uint32_t connId;
    bool replace;
    std::vector<std::string> tags;
    std::string moduleName;
    std::string connName;
  };
  std::vector<ResolvedPointTable> pointTables;
  pointTables.reserve(static_cast<size_t>(config.point_tables_size()));

  for (const auto &table : config.point_tables()) {
    if (!ValidateConnKey(table.module_name(), table.conn_name())) {
      LOG_ERROR("DataCenter point table missing module_name/conn_name");
      ok = false;
      continue;
    }
    if (table.tags().empty()) {
      LOG_ERROR("DataCenter point table missing tags: module_name={}, conn_name={}", table.module_name(), table.conn_name());
      ok = false;
      continue;
    }

    uint32_t connId = 0;
    if (!ResolveConnId(connIds, table.module_name(), table.conn_name(), &connId)) {
      LOG_ERROR("DataCenter connection not found: module_name={}, conn_name={}", table.module_name(), table.conn_name());
      ok = false;
      continue;
    }

    bool tagsOk = true;
    std::vector<std::string> tags;
    tags.reserve(static_cast<size_t>(table.tags_size()));
    for (const auto &tag : table.tags()) {
      if (tag.empty()) {
        LOG_ERROR("DataCenter point table has empty tag: module_name={}, conn_name={}", table.module_name(), table.conn_name());
        tagsOk = false;
        continue;
      }
      tags.emplace_back(tag);
    }
    if (!tagsOk) {
      ok = false;
      continue;
    }

    pointTables.push_back(ResolvedPointTable{
        .connId = connId,
        .replace = table.replace(),
        .tags = std::move(tags),
        .moduleName = table.module_name(),
        .connName = table.conn_name()});
  }

  std::vector<DataCenterProto::Route> routes;
  const bool hasRoutes = config.has_routes() && config.routes().routes_size() > 0;
  if (hasRoutes) {
    routes.reserve(static_cast<size_t>(config.routes().routes_size()));
    for (const auto &route : config.routes().routes()) {
      if (!route.has_src() || !route.has_dst()) {
        LOG_ERROR("DataCenter route missing src/dst");
        ok = false;
        continue;
      }
      const auto &src = route.src();
      const auto &dst = route.dst();
      if (!ValidateConnKey(src.module_name(), src.conn_name()) || !ValidateConnKey(dst.module_name(), dst.conn_name())) {
        LOG_ERROR("DataCenter route missing module_name/conn_name");
        ok = false;
        continue;
      }
      if (src.tag().empty() || dst.tag().empty()) {
        LOG_ERROR("DataCenter route missing tag: src={}:{}, dst={}:{}",
                  src.module_name(),
                  src.conn_name(),
                  dst.module_name(),
                  dst.conn_name());
        ok = false;
        continue;
      }

      uint32_t srcConnId = 0;
      uint32_t dstConnId = 0;
      if (!ResolveConnId(connIds, src.module_name(), src.conn_name(), &srcConnId)) {
        LOG_ERROR("DataCenter route src connection not found: module_name={}, conn_name={}",
                  src.module_name(),
                  src.conn_name());
        ok = false;
        continue;
      }
      if (!ResolveConnId(connIds, dst.module_name(), dst.conn_name(), &dstConnId)) {
        LOG_ERROR("DataCenter route dst connection not found: module_name={}, conn_name={}",
                  dst.module_name(),
                  dst.conn_name());
        ok = false;
        continue;
      }

      DataCenterProto::Route resolved;
      resolved.mutable_src()->set_conn_id(srcConnId);
      resolved.mutable_src()->set_tag(src.tag());
      resolved.mutable_dst()->set_conn_id(dstConnId);
      resolved.mutable_dst()->set_tag(dst.tag());
      routes.emplace_back(std::move(resolved));
    }
  }

  if (!ok) {
    LOG_ERROR("DataCenter config validation failed; aborting push");
    return false;
  }

  for (const auto &table : pointTables) {
    DataCenterProto::UpsertPointTableRequest req;
    req.set_conn_id(table.connId);
    req.set_replace(table.replace);
    for (const auto &tag : table.tags) {
      req.add_tags(tag);
    }

    LOG_INFO("Pushing DataCenter point table: module_name={}, conn_name={}, tags={}, replace={}",
             table.moduleName,
             table.connName,
             req.tags_size(),
             req.replace());
    grpc::ClientContext ctx;
    DataCenterProto::Empty resp;
    status = stub->UpsertPointTable(&ctx, req, &resp);
    if (!status.ok()) {
      LOG_ERROR("DataCenter point table push failed: module_name={}, conn_name={}, reason={}",
                table.moduleName,
                table.connName,
                status.error_message());
      return false;
    }
    LOG_INFO("DataCenter point table pushed: module_name={}, conn_name={}, tags={}",
             table.moduleName,
             table.connName,
             req.tags_size());
  }

  if (hasRoutes && !routes.empty()) {
    DataCenterProto::UpsertRoutesRequest req;
    req.set_replace(config.routes().replace());
    for (const auto &route : routes) {
      *req.add_routes() = route;
    }

    LOG_INFO("Pushing DataCenter routes: routes={}, replace={}", req.routes_size(), req.replace());
    grpc::ClientContext ctx;
    DataCenterProto::Empty resp;
    status = stub->UpsertRoutes(&ctx, req, &resp);
    if (!status.ok()) {
      LOG_ERROR("DataCenter routes push failed: {}", status.error_message());
      return false;
    }
    LOG_INFO("DataCenter routes pushed: routes={}", req.routes_size());
  }

  return true;
}
}  // namespace ConfigPusher
