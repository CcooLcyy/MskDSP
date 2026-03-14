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
}  // namespace DataCenter
