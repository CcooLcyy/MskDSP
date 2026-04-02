#include "ModbusRTULinkStore.h"

#include <unordered_set>
#include <utility>

#include "mskdsp/detail/ProtoFileStore.hpp"

namespace ModbusRTU {
namespace {
grpc::Status validateLinksConfig(const ModbusRTUProto::LinksConfig& config) {
  std::unordered_set<std::string> connNames;
  std::unordered_set<uint32_t> connIds;
  for (const auto& link : config.links()) {
    if (!link.has_config()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "links 包含空 config");
    }
    if (link.config().conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "links 包含空 conn_name");
    }
    if (link.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "links 包含 conn_id=0");
    }
    if (!connNames.emplace(link.config().conn_name()).second) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "links 包含重复 conn_name");
    }
    if (!connIds.emplace(link.conn_id()).second) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "links 包含重复 conn_id");
    }
  }
  return grpc::Status::OK;
}
}  // namespace

ModbusRTULinkStore::ModbusRTULinkStore(std::filesystem::path linksPath) :
  linksPath_(std::move(linksPath)) {}

grpc::Status ModbusRTULinkStore::Save(const ModbusRTUProto::LinksConfig& config) {
  mskdsp::detail::ProtoFileStore<ModbusRTUProto::LinksConfig> store(linksPath_, validateLinksConfig);
  return store.Save(config);
}

grpc::Status ModbusRTULinkStore::Load(ModbusRTUProto::LinksConfig* out) {
  mskdsp::detail::ProtoFileStore<ModbusRTUProto::LinksConfig> store(linksPath_, validateLinksConfig);
  return store.Load(out);
}

std::filesystem::path ModbusRTULinkStore::linksPath() const {
  return linksPath_;
}

std::filesystem::path ModbusRTULinkStore::backupPath() const {
  mskdsp::detail::ProtoFileStore<ModbusRTUProto::LinksConfig> store(linksPath_, validateLinksConfig);
  return store.backupPath();
}

std::filesystem::path ModbusRTULinkStore::tmpPath() const {
  mskdsp::detail::ProtoFileStore<ModbusRTUProto::LinksConfig> store(linksPath_, validateLinksConfig);
  return store.tmpPath();
}
}  // namespace ModbusRTU
