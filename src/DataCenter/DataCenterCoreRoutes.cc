#include "DataCenterCore.h"

#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DataCenter {
grpc::Status DataCenterCore::UpsertRoutes(const DataCenterProto::UpsertRoutesRequest &request) {
  std::unordered_map<StableEndpointKey, StableEndpointKeySet, StableEndpointKeyHash> next;
  if (!request.replace()) {
    next = routes_;
  }

  for (const auto &route : request.routes()) {
    StableEndpointKey src;
    uint32_t srcConnId = 0;
    auto status = resolveEndpoint(route.src(), &src, &srcConnId);
    if (!status.ok()) {
      return status;
    }
    StableEndpointKey dst;
    uint32_t dstConnId = 0;
    status = resolveEndpoint(route.dst(), &dst, &dstConnId);
    if (!status.ok()) {
      return status;
    }
    if (srcConnId != 0) {
      status = validateEndpointAgainstConnTags(srcConnId, src.tag);
      if (!status.ok()) {
        return status;
      }
    }
    if (dstConnId != 0) {
      status = validateEndpointAgainstConnTags(dstConnId, dst.tag);
      if (!status.ok()) {
        return status;
      }
    }

    next[std::move(src)].emplace(std::move(dst));
  }
  routes_ = std::move(next);
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::DeleteRoutes(const DataCenterProto::DeleteRoutesRequest &request) {
  auto next = routes_;
  for (const auto &route : request.routes()) {
    StableEndpointKey src;
    auto status = resolveEndpoint(route.src(), &src, nullptr);
    if (!status.ok()) {
      return status;
    }
    StableEndpointKey dst;
    status = resolveEndpoint(route.dst(), &dst, nullptr);
    if (!status.ok()) {
      return status;
    }

    auto srcIt = next.find(src);
    if (srcIt == next.end()) {
      continue;
    }
    srcIt->second.erase(dst);
    if (srcIt->second.empty()) {
      next.erase(srcIt);
    }
  }
  routes_ = std::move(next);
  return grpc::Status::OK;
}

DataCenterProto::ListRoutesResponse DataCenterCore::ListRoutes(const DataCenterProto::ListRoutesRequest &request) const {
  std::vector<DataCenterProto::Route> tmp;
  for (const auto &[src, dstSet] : routes_) {
    uint32_t srcConnId = 0;
    (void)tryResolveConnId(src, &srcConnId);
    if (request.src_conn_id() != 0 && request.src_conn_id() != srcConnId) {
      continue;
    }
    if (!request.src_tag().empty() && request.src_tag() != src.tag) {
      continue;
    }
    for (const auto &dst : dstSet) {
      uint32_t dstConnId = 0;
      (void)tryResolveConnId(dst, &dstConnId);
      if (request.dst_conn_id() != 0 && request.dst_conn_id() != dstConnId) {
        continue;
      }
      if (!request.dst_tag().empty() && request.dst_tag() != dst.tag) {
        continue;
      }
      DataCenterProto::Route route;
      *route.mutable_src() = dumpEndpoint(src);
      *route.mutable_dst() = dumpEndpoint(dst);
      tmp.emplace_back(std::move(route));
    }
  }

  std::sort(tmp.begin(), tmp.end(), [](const auto &a, const auto &b) {
    return std::make_tuple(a.src().module_name(), a.src().conn_name(), a.src().tag(),
                           a.dst().module_name(), a.dst().conn_name(), a.dst().tag()) <
        std::make_tuple(b.src().module_name(), b.src().conn_name(), b.src().tag(),
                        b.dst().module_name(), b.dst().conn_name(), b.dst().tag());
  });

  DataCenterProto::ListRoutesResponse resp;
  for (const auto &route : tmp) {
    *resp.add_routes() = route;
  }
  return resp;
}

grpc::Status DataCenterCore::ReplaceRoutesConfig(const DataCenterProto::RoutesConfig &config) {
  std::unordered_map<StableEndpointKey, StableEndpointKeySet, StableEndpointKeyHash> next;
  for (const auto &route : config.routes()) {
    StableEndpointKey src;
    uint32_t srcConnId = 0;
    auto status = resolveEndpoint(route.src(), &src, &srcConnId);
    if (!status.ok()) {
      return status;
    }
    StableEndpointKey dst;
    uint32_t dstConnId = 0;
    status = resolveEndpoint(route.dst(), &dst, &dstConnId);
    if (!status.ok()) {
      return status;
    }
    if (srcConnId != 0) {
      status = validateEndpointAgainstConnTags(srcConnId, src.tag);
      if (!status.ok()) {
        return status;
      }
    }
    if (dstConnId != 0) {
      status = validateEndpointAgainstConnTags(dstConnId, dst.tag);
      if (!status.ok()) {
        return status;
      }
    }

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
