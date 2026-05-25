#include "DataCenterStateStore.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "mskdsp/detail/ProtoFileStore.hpp"

namespace DataCenter {
namespace {
constexpr uint32_t kDataCenterStateSchemaVersion = 1;
constexpr size_t kHashCombineMagic = static_cast<size_t>(0x9e3779b9);

inline size_t hashCombine(size_t seed, size_t value) noexcept {
  return seed ^ (value + kHashCombineMagic + (seed << 6) + (seed >> 2));
}

struct StringPairHash {
  size_t operator()(const std::pair<std::string, std::string>& key) const noexcept {
    size_t h1 = std::hash<std::string>{}(key.first);
    size_t h2 = std::hash<std::string>{}(key.second);
    return hashCombine(h1, h2);
  }
};

using StableConnKey = std::pair<std::string, std::string>;
using StableConnKeySet = std::unordered_set<StableConnKey, StringPairHash>;
using TagsByStableConnKey = std::unordered_map<StableConnKey, std::unordered_set<std::string>, StringPairHash>;

grpc::Status validateConnectionsConfig(const DataCenterProto::ConnectionsConfig& config) {
  std::unordered_set<uint32_t> ids;
  std::unordered_set<std::pair<std::string, std::string>, StringPairHash> keys;

  for (const auto& conn : config.conns()) {
    if (conn.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "state.connections 包含 conn_id=0");
    }
    if (conn.module_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "state.connections 包含空 module_name");
    }
    if (conn.conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "state.connections 包含空 conn_name");
    }

    auto [_, idInserted] = ids.emplace(conn.conn_id());
    if (!idInserted) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "state.connections 包含重复的 conn_id");
    }

    auto [__, keyInserted] = keys.emplace(conn.module_name(), conn.conn_name());
    if (!keyInserted) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "state.connections 包含重复的稳定连接主键");
    }
  }

  return grpc::Status::OK;
}

grpc::Status validateConnTagsConfig(const DataCenterProto::ConnTagsConfig& config, const StableConnKeySet& connectionKeys,
                                    TagsByStableConnKey* tagsByKey) {
  if (tagsByKey == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tagsByKey 为空");
  }
  std::unordered_set<std::pair<std::string, std::string>, StringPairHash> keys;
  for (const auto& table : config.conn_tags()) {
    if (table.module_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "state.conn_tags 包含空 module_name");
    }
    if (table.conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "state.conn_tags 包含空 conn_name");
    }
    StableConnKey key{table.module_name(), table.conn_name()};
    auto [_, inserted] = keys.emplace(key);
    if (!inserted) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "state.conn_tags 包含重复的稳定连接主键");
    }
    if (!connectionKeys.contains(key)) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "state.conn_tags 引用不存在的稳定连接主键");
    }
    auto& tags = (*tagsByKey)[std::move(key)];
    for (const auto& tag : table.tags()) {
      if (tag.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "state.conn_tags 包含空 tag");
      }
      if (!tags.emplace(tag).second) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "state.conn_tags 包含重复 tag");
      }
    }
  }
  return grpc::Status::OK;
}

grpc::Status validateRouteEndpoint(const DataCenterProto::Endpoint& endpoint, const char* direction,
                                   const StableConnKeySet& connectionKeys, const TagsByStableConnKey& tagsByKey) {
  const std::string prefix = std::string("state.routes ") + direction;
  if (endpoint.tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, prefix + " tag 为空");
  }
  if (endpoint.module_name().empty() || endpoint.conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, prefix + " 稳定连接主键为空");
  }
  StableConnKey key{endpoint.module_name(), endpoint.conn_name()};
  if (!connectionKeys.contains(key)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, prefix + " 稳定连接主键不存在");
  }
  auto tagsIt = tagsByKey.find(key);
  if (tagsIt != tagsByKey.end() && !tagsIt->second.contains(endpoint.tag())) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, prefix + " tag 未在对应 conn_tags 中注册");
  }
  return grpc::Status::OK;
}

grpc::Status validateRoutesConfig(const DataCenterProto::RoutesConfig& config, const StableConnKeySet& connectionKeys,
                                  const TagsByStableConnKey& tagsByKey) {
  for (const auto& route : config.routes()) {
    auto status = validateRouteEndpoint(route.src(), "src", connectionKeys, tagsByKey);
    if (!status.ok()) {
      return status;
    }
    status = validateRouteEndpoint(route.dst(), "dst", connectionKeys, tagsByKey);
    if (!status.ok()) {
      return status;
    }
  }
  return grpc::Status::OK;
}

grpc::Status validateState(const DataCenterProto::DataCenterState& state) {
  if (state.schema_version() != kDataCenterStateSchemaVersion) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "state.schema_version 不支持");
  }
  auto status = validateConnectionsConfig(state.connections());
  if (!status.ok()) {
    return status;
  }
  StableConnKeySet connectionKeys;
  for (const auto& conn : state.connections().conns()) {
    connectionKeys.emplace(conn.module_name(), conn.conn_name());
  }
  TagsByStableConnKey tagsByKey;
  status = validateConnTagsConfig(state.conn_tags(), connectionKeys, &tagsByKey);
  if (!status.ok()) {
    return status;
  }
  return validateRoutesConfig(state.routes(), connectionKeys, tagsByKey);
}

DataCenterProto::DataCenterState normalizeState(DataCenterProto::DataCenterState state) {
  if (state.schema_version() == 0) {
    state.set_schema_version(kDataCenterStateSchemaVersion);
  }
  return state;
}
}  // namespace

DataCenterStateStore::DataCenterStateStore(std::filesystem::path statePath) :
  statePath_(std::move(statePath)) {}

grpc::Status DataCenterStateStore::Save(const DataCenterProto::DataCenterState& state, TraceFn trace) {
  auto normalized = normalizeState(state);
  mskdsp::detail::ProtoFileStore<DataCenterProto::DataCenterState> store(statePath_,
                                                                         validateState,
                                                                         std::move(trace));
  return store.Save(normalized);
}

grpc::Status DataCenterStateStore::Load(DataCenterProto::DataCenterState* out, TraceFn trace) {
  mskdsp::detail::ProtoFileStore<DataCenterProto::DataCenterState> store(statePath_,
                                                                         validateState,
                                                                         std::move(trace));
  return store.Load(out);
}

std::filesystem::path DataCenterStateStore::statePath() const {
  return statePath_;
}

std::filesystem::path DataCenterStateStore::backupPath() const {
  mskdsp::detail::ProtoFileStore<DataCenterProto::DataCenterState> store(statePath_,
                                                                         validateState);
  return store.backupPath();
}

std::filesystem::path DataCenterStateStore::tmpPath() const {
  mskdsp::detail::ProtoFileStore<DataCenterProto::DataCenterState> store(statePath_,
                                                                         validateState);
  return store.tmpPath();
}
}  // namespace DataCenter
