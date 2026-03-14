#include "DataCenterCore.h"

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

namespace DataCenter {
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
    status = validateEndpointAgainstConnTags(route.src().conn_id(), route.src().tag());
    if (!status.ok()) {
      return status;
    }
    status = validateEndpointAgainstConnTags(route.dst().conn_id(), route.dst().tag());
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
    status = validateEndpointAgainstConnTags(route.src().conn_id(), route.src().tag());
    if (!status.ok()) {
      return status;
    }
    status = validateEndpointAgainstConnTags(route.dst().conn_id(), route.dst().tag());
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
}  // namespace DataCenter
