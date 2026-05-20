#include "DataCenterCore.h"

#include <cstddef>
#include <functional>

namespace DataCenter {
namespace {
constexpr size_t kHashCombineMagic = static_cast<size_t>(0x9e3779b9);

inline size_t hashCombine(size_t seed, size_t value) noexcept {
  return seed ^ (value + kHashCombineMagic + (seed << 6) + (seed >> 2));
}
}  // namespace

bool DataCenterCore::ConnKey::operator==(const ConnKey &other) const {
  return moduleName == other.moduleName && connName == other.connName;
}

size_t DataCenterCore::ConnKeyHash::operator()(const ConnKey &key) const noexcept {
  size_t h1 = std::hash<std::string>{}(key.moduleName);
  size_t h2 = std::hash<std::string>{}(key.connName);
  return hashCombine(h1, h2);
}

bool DataCenterCore::EndpointKey::operator==(const EndpointKey &other) const {
  return connId == other.connId && tag == other.tag;
}

size_t DataCenterCore::EndpointKeyHash::operator()(const EndpointKey &key) const noexcept {
  size_t h1 = std::hash<uint32_t>{}(key.connId);
  size_t h2 = std::hash<std::string>{}(key.tag);
  return hashCombine(h1, h2);
}

bool DataCenterCore::StableEndpointKey::operator==(const StableEndpointKey &other) const {
  return moduleName == other.moduleName && connName == other.connName && tag == other.tag;
}

size_t DataCenterCore::StableEndpointKeyHash::operator()(const StableEndpointKey &key) const noexcept {
  size_t h1 = std::hash<std::string>{}(key.moduleName);
  size_t h2 = std::hash<std::string>{}(key.connName);
  size_t h3 = std::hash<std::string>{}(key.tag);
  return hashCombine(hashCombine(h1, h2), h3);
}

grpc::Status DataCenterCore::validateConnKey(const DataCenterProto::ConnectionKey &key) {
  if (key.module_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "module_name 不能为空");
  }
  if (key.conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::validateEndpoint(uint32_t connId, const std::string &tag) {
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  if (tag.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag 不能为空");
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::validateEndpointAgainstConnTags(uint32_t connId, const std::string &tag) const {
  auto it = connTagsByConnId_.find(connId);
  if (it == connTagsByConnId_.end()) {
    return grpc::Status::OK;
  }
  if (!it->second.contains(tag)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "连接标签注册表中未找到 tag");
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::resolveEndpoint(const DataCenterProto::Endpoint &endpoint, StableEndpointKey *out, uint32_t *resolvedConnId) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (endpoint.tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag 不能为空");
  }

  if (!endpoint.module_name().empty() || !endpoint.conn_name().empty()) {
    if (endpoint.module_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "module_name 不能为空");
    }
    if (endpoint.conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
    }
    *out = StableEndpointKey{endpoint.module_name(), endpoint.conn_name(), endpoint.tag()};
    if (endpoint.conn_id() != 0) {
      auto connIt = connections_.find(endpoint.conn_id());
      if (connIt != connections_.end() &&
          (connIt->second.module_name() != endpoint.module_name() || connIt->second.conn_name() != endpoint.conn_name())) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 与稳定连接主键不一致");
      }
    }
    if (resolvedConnId != nullptr) {
      uint32_t connId = 0;
      const bool resolvedByStableKey = tryResolveConnId(*out, &connId);
      if (endpoint.conn_id() != 0) {
        auto connIt = connections_.find(endpoint.conn_id());
        if (!resolvedByStableKey && connIt != connections_.end()) {
          connId = endpoint.conn_id();
        }
      }
      *resolvedConnId = connId;
    }
    return grpc::Status::OK;
  }

  auto status = validateEndpoint(endpoint.conn_id(), endpoint.tag());
  if (!status.ok()) {
    return status;
  }
  auto connIt = connections_.find(endpoint.conn_id());
  if (connIt == connections_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "conn_id 未在连接注册表中找到，无法转换为稳定路由端点");
  }
  *out = StableEndpointKey{connIt->second.module_name(), connIt->second.conn_name(), endpoint.tag()};
  if (resolvedConnId != nullptr) {
    *resolvedConnId = endpoint.conn_id();
  }
  return grpc::Status::OK;
}

bool DataCenterCore::tryResolveConnId(const StableEndpointKey &endpoint, uint32_t *outConnId) const {
  ConnKey key{endpoint.moduleName, endpoint.connName};
  auto it = connIdsByKey_.find(key);
  if (it == connIdsByKey_.end()) {
    return false;
  }
  if (outConnId != nullptr) {
    *outConnId = it->second;
  }
  return true;
}

DataCenterProto::Endpoint DataCenterCore::dumpEndpoint(const StableEndpointKey &endpoint) const {
  DataCenterProto::Endpoint out;
  out.set_module_name(endpoint.moduleName);
  out.set_conn_name(endpoint.connName);
  out.set_tag(endpoint.tag);
  uint32_t connId = 0;
  if (tryResolveConnId(endpoint, &connId)) {
    out.set_conn_id(connId);
  }
  return out;
}
}  // namespace DataCenter
