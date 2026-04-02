#include "DLT645LinkStore.h"

#include <unordered_set>
#include <utility>

#include "mskdsp/detail/ProtoFileStore.hpp"

namespace DLT645 {
namespace {
grpc::Status validateLinksConfig(const DLT645Proto::LinksConfig &config) {
  std::unordered_set<std::string> names;
  std::unordered_set<uint32_t> connIds;
  for (const auto &link : config.links()) {
    if (!link.has_config()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "links 包含空 config");
    }
    if (link.config().conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "links 包含空 conn_name");
    }
    if (link.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "links 包含 conn_id=0");
    }
    if (!names.emplace(link.config().conn_name()).second) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "links 包含重复 conn_name");
    }
    if (!connIds.emplace(link.conn_id()).second) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "links 包含重复 conn_id");
    }
  }
  return grpc::Status::OK;
}
}  // namespace

DLT645LinkStore::DLT645LinkStore(std::filesystem::path linksPath) :
  linksPath_(std::move(linksPath)) {}

grpc::Status DLT645LinkStore::Save(const DLT645Proto::LinksConfig &config) {
  mskdsp::detail::ProtoFileStore<DLT645Proto::LinksConfig> store(linksPath_, validateLinksConfig);
  return store.Save(config);
}

grpc::Status DLT645LinkStore::Load(DLT645Proto::LinksConfig *out) {
  mskdsp::detail::ProtoFileStore<DLT645Proto::LinksConfig> store(linksPath_, validateLinksConfig);
  return store.Load(out);
}

std::filesystem::path DLT645LinkStore::linksPath() const {
  return linksPath_;
}

std::filesystem::path DLT645LinkStore::backupPath() const {
  mskdsp::detail::ProtoFileStore<DLT645Proto::LinksConfig> store(linksPath_, validateLinksConfig);
  return store.backupPath();
}

std::filesystem::path DLT645LinkStore::tmpPath() const {
  mskdsp::detail::ProtoFileStore<DLT645Proto::LinksConfig> store(linksPath_, validateLinksConfig);
  return store.tmpPath();
}
}  // namespace DLT645
