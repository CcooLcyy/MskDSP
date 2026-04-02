#include "IEC104LinkStore.h"

#include <string>
#include <unordered_set>
#include <utility>

#include "IEC104LinkManager.h"
#include "mskdsp/detail/ProtoFileStore.hpp"

namespace IEC104 {

IEC104LinkStore::IEC104LinkStore(std::filesystem::path linksPath) :
  linksPath_(std::move(linksPath)) {}

grpc::Status IEC104LinkStore::Save(const IEC104Proto::LinksConfig& config) {
  mskdsp::detail::ProtoFileStore<IEC104Proto::LinksConfig> store(linksPath_, ValidateLinksConfig);
  return store.Save(config);
}

grpc::Status IEC104LinkStore::Load(IEC104Proto::LinksConfig* out) {
  mskdsp::detail::ProtoFileStore<IEC104Proto::LinksConfig> store(linksPath_, ValidateLinksConfig);
  return store.Load(out);
}

std::filesystem::path IEC104LinkStore::linksPath() const {
  return linksPath_;
}

std::filesystem::path IEC104LinkStore::backupPath() const {
  mskdsp::detail::ProtoFileStore<IEC104Proto::LinksConfig> store(linksPath_, ValidateLinksConfig);
  return store.backupPath();
}

std::filesystem::path IEC104LinkStore::tmpPath() const {
  mskdsp::detail::ProtoFileStore<IEC104Proto::LinksConfig> store(linksPath_, ValidateLinksConfig);
  return store.tmpPath();
}

grpc::Status IEC104LinkStore::ValidateLinksConfig(const IEC104Proto::LinksConfig& config) {
  std::unordered_set<std::string> connNames;
  for (const auto& link : config.links()) {
    if (link.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "links 包含 conn_id=0");
    }
    auto status = LinkManager::validateLinkConfig(link.config());
    if (!status.ok()) {
      return status;
    }
    auto [_, inserted] = connNames.emplace(link.config().conn_name());
    if (!inserted) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "links 包含重复的 conn_name");
    }
  }
  return grpc::Status::OK;
}

}  // namespace IEC104
