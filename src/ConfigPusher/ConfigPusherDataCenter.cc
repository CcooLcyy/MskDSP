#include "ConfigPusherDataCenter.h"

#include <grpcpp/client_context.h>
#include <google/protobuf/message.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Logger.h"
#include "ProtoLogUtil.hpp"

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
    LOG_ERROR("DataCenter gRPC stub 为空");
    return false;
  }
  if (!HasDataCenterConfig(config)) {
    LOG_INFO("DataCenter 配置为空，跳过下发");
    return true;
  }

  DataCenterProto::ListConnectionsResponse connections;
  grpc::ClientContext listCtx;
  DataCenterProto::Empty listReq;
  LOG_INFO("发送 DataCenter 获取连接列表请求报文: {}", formatProtoForLog(listReq));
  auto status = stub->ListConnections(&listCtx, listReq, &connections);
  if (!status.ok()) {
    LOG_ERROR("获取 DataCenter 连接列表失败: 请求={}, 原因={}", formatProtoForLog(listReq), status.error_message());
    return false;
  }
  LOG_INFO("收到 DataCenter 获取连接列表响应报文: {}", formatProtoForLog(connections));

  std::unordered_map<ConnKey, uint32_t, ConnKeyHash> connIds;
  connIds.reserve(static_cast<size_t>(connections.conns_size()));
  for (const auto &conn : connections.conns()) {
    if (conn.module_name().empty() || conn.conn_name().empty() || conn.conn_id() == 0) {
      continue;
    }
    connIds.emplace(ConnKey{conn.module_name(), conn.conn_name()}, conn.conn_id());
  }

  bool ok = true;
  struct ResolvedConnTags {
    uint32_t connId;
    bool replace;
    std::vector<std::string> tags;
    std::string moduleName;
    std::string connName;
  };
  std::vector<ResolvedConnTags> connTagsList;
  connTagsList.reserve(static_cast<size_t>(config.point_tables_size()));

  for (const auto &table : config.point_tables()) {
    if (!ValidateConnKey(table.module_name(), table.conn_name())) {
      LOG_ERROR("DataCenter 连接标签注册表配置缺少 模块名/连接名");
      ok = false;
      continue;
    }
    if (table.tags().empty()) {
      LOG_ERROR("DataCenter 连接标签注册表配置缺少标签: 模块名={}, 连接名={}", table.module_name(), table.conn_name());
      ok = false;
      continue;
    }

    uint32_t connId = 0;
    if (!ResolveConnId(connIds, table.module_name(), table.conn_name(), &connId)) {
      LOG_ERROR("DataCenter 未找到连接: 模块名={}, 连接名={}", table.module_name(), table.conn_name());
      ok = false;
      continue;
    }

    bool tagsOk = true;
    std::vector<std::string> tags;
    tags.reserve(static_cast<size_t>(table.tags_size()));
    for (const auto &tag : table.tags()) {
      if (tag.empty()) {
        LOG_ERROR("DataCenter 连接标签注册表配置包含空标签: 模块名={}, 连接名={}", table.module_name(), table.conn_name());
        tagsOk = false;
        continue;
      }
      tags.emplace_back(tag);
    }
    if (!tagsOk) {
      ok = false;
      continue;
    }

    connTagsList.push_back(ResolvedConnTags{
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
        LOG_ERROR("DataCenter 路由配置缺少 src/dst");
        ok = false;
        continue;
      }
      const auto &src = route.src();
      const auto &dst = route.dst();
      if (!ValidateConnKey(src.module_name(), src.conn_name()) || !ValidateConnKey(dst.module_name(), dst.conn_name())) {
        LOG_ERROR("DataCenter 路由配置缺少 模块名/连接名");
        ok = false;
        continue;
      }
      if (src.tag().empty() || dst.tag().empty()) {
        LOG_ERROR("DataCenter 路由配置缺少标签: 源模块名={}, 源连接名={}, 目标模块名={}, 目标连接名={}",
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
        LOG_ERROR("DataCenter 未找到路由源连接: 模块名={}, 连接名={}",
                  src.module_name(),
                  src.conn_name());
        ok = false;
        continue;
      }
      if (!ResolveConnId(connIds, dst.module_name(), dst.conn_name(), &dstConnId)) {
        LOG_ERROR("DataCenter 未找到路由目的连接: 模块名={}, 连接名={}",
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
    LOG_ERROR("DataCenter 配置校验失败，终止下发");
    return false;
  }

  for (const auto &table : connTagsList) {
    DataCenterProto::UpsertConnTagsRequest req;
    req.set_conn_id(table.connId);
    req.set_replace(table.replace);
    for (const auto &tag : table.tags) {
      req.add_tags(tag);
    }

    LOG_INFO("开始下发 DataCenter 连接标签注册表: 模块名={}, 连接名={}, 标签数={}, 是否替换={}",
             table.moduleName,
             table.connName,
             req.tags_size(),
             req.replace());
    LOG_INFO("发送 DataCenter 连接标签注册表请求报文: {}", formatProtoForLog(req));
    grpc::ClientContext ctx;
    DataCenterProto::Empty resp;
    status = stub->UpsertConnTags(&ctx, req, &resp);
    if (!status.ok()) {
      LOG_ERROR("DataCenter 连接标签注册表下发失败: 模块名={}, 连接名={}, 请求={}, 原因={}",
                table.moduleName,
                table.connName,
                formatProtoForLog(req),
                status.error_message());
      return false;
    }
    LOG_INFO("收到 DataCenter 连接标签注册表响应报文: {}", formatProtoForLog(resp));
    LOG_INFO("DataCenter 连接标签注册表下发成功: 模块名={}, 连接名={}, 标签数={}",
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

    LOG_INFO("开始下发 DataCenter 路由: 路由数={}, 是否替换={}", req.routes_size(), req.replace());
    LOG_INFO("发送 DataCenter 路由请求报文: {}", formatProtoForLog(req));
    grpc::ClientContext ctx;
    DataCenterProto::Empty resp;
    status = stub->UpsertRoutes(&ctx, req, &resp);
    if (!status.ok()) {
      LOG_ERROR("DataCenter 路由下发失败: 请求={}, 原因={}", formatProtoForLog(req), status.error_message());
      return false;
    }
    LOG_INFO("收到 DataCenter 路由响应报文: {}", formatProtoForLog(resp));
    LOG_INFO("DataCenter 路由下发成功: 路由数={}", req.routes_size());
  }

  return true;
}
}  // namespace ConfigPusher
