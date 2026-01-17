#include "DataCenterConnectionStore.h"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>

#include "detail/ProtoFileStore.hpp"

namespace DataCenter {
namespace {
constexpr size_t kHashCombineMagic = static_cast<size_t>(0x9e3779b9);

inline size_t hashCombine(size_t seed, size_t value) noexcept {
  return seed ^ (value + kHashCombineMagic + (seed << 6) + (seed >> 2));
}

struct ConnectionKeyHash {
  size_t operator()(const std::pair<std::string, std::string>& key) const noexcept {
    size_t h1 = std::hash<std::string>{}(key.first);
    size_t h2 = std::hash<std::string>{}(key.second);
    return hashCombine(h1, h2);
  }
};

grpc::Status validateConnectionsConfig(const DataCenterProto::ConnectionsConfig& config) {
  std::unordered_set<uint32_t> ids;
  std::unordered_set<std::pair<std::string, std::string>, ConnectionKeyHash> keys;

  for (const auto& conn : config.conns()) {
    if (conn.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含 conn_id=0");
    }
    if (conn.module_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含空 module_name");
    }
    if (conn.conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含空 conn_name");
    }

    auto [_, idInserted] = ids.emplace(conn.conn_id());
    if (!idInserted) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含重复的 conn_id");
    }

    auto [__, keyInserted] = keys.emplace(conn.module_name(), conn.conn_name());
    if (!keyInserted) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含重复的 (module_name, conn_name)");
    }
  }

  return grpc::Status::OK;
}
}  // namespace

DataCenterConnectionStore::DataCenterConnectionStore(std::filesystem::path connectionsPath) :
  connectionsPath_(std::move(connectionsPath)) {}

grpc::Status DataCenterConnectionStore::Save(const DataCenterProto::ConnectionsConfig& config) {
  detail::ProtoFileStore<DataCenterProto::ConnectionsConfig> store(connectionsPath_, validateConnectionsConfig);
  return store.Save(config);
}

grpc::Status DataCenterConnectionStore::Load(DataCenterProto::ConnectionsConfig* out) {
  detail::ProtoFileStore<DataCenterProto::ConnectionsConfig> store(connectionsPath_, validateConnectionsConfig);
  return store.Load(out);
}

std::filesystem::path DataCenterConnectionStore::connectionsPath() const {
  return connectionsPath_;
}

std::filesystem::path DataCenterConnectionStore::backupPath() const {
  detail::ProtoFileStore<DataCenterProto::ConnectionsConfig> store(connectionsPath_, validateConnectionsConfig);
  return store.backupPath();
}

std::filesystem::path DataCenterConnectionStore::tmpPath() const {
  detail::ProtoFileStore<DataCenterProto::ConnectionsConfig> store(connectionsPath_, validateConnectionsConfig);
  return store.tmpPath();
}
}  // namespace DataCenter
