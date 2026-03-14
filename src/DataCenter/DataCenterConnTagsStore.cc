#include "DataCenterConnTagsStore.h"

#include <system_error>

#include "Logger.h"
#include "detail/ProtoFileStore.hpp"

namespace DataCenter {
namespace {
constexpr char kConnTagsFileName[] = "conn_tags.pb";
constexpr char kLegacyConnTagsFileName[] = "point_tables.pb";

grpc::Status validateConnTagsConfig(const DataCenterProto::ConnTagsConfig& config) {
  for (const auto& table : config.conn_tags()) {
    if (table.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_tags 包含 conn_id=0");
    }
    for (const auto& tag : table.tags()) {
      if (tag.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_tags 包含空 tag");
      }
    }
  }
  return grpc::Status::OK;
}

bool hasStoreFiles(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) || std::filesystem::exists(std::filesystem::path(path.string() + ".bak"), ec);
}

std::filesystem::path legacyConnTagsPathFor(const std::filesystem::path& path) {
  if (path.filename() != kConnTagsFileName) {
    return {};
  }
  return path.parent_path() / kLegacyConnTagsFileName;
}
}  // namespace

DataCenterConnTagsStore::DataCenterConnTagsStore(std::filesystem::path connTagsPath) :
  connTagsPath_(std::move(connTagsPath)) {}

grpc::Status DataCenterConnTagsStore::Save(const DataCenterProto::ConnTagsConfig& config) {
  detail::ProtoFileStore<DataCenterProto::ConnTagsConfig> store(connTagsPath_, validateConnTagsConfig);
  return store.Save(config);
}

grpc::Status DataCenterConnTagsStore::Load(DataCenterProto::ConnTagsConfig* out) {
  detail::ProtoFileStore<DataCenterProto::ConnTagsConfig> store(connTagsPath_, validateConnTagsConfig);
  const bool hasCurrentFiles = hasStoreFiles(connTagsPath_);
  auto status = store.Load(out);
  if (!status.ok() || hasCurrentFiles) {
    return status;
  }

  const auto legacyPath = legacyConnTagsPathFor(connTagsPath_);
  if (legacyPath.empty() || !hasStoreFiles(legacyPath)) {
    return status;
  }

  detail::ProtoFileStore<DataCenterProto::ConnTagsConfig> legacyStore(legacyPath, validateConnTagsConfig);
  auto legacyStatus = legacyStore.Load(out);
  if (legacyStatus.ok()) {
    LOG_INFO("DataCenter 连接标签注册表按历史文件名兼容路径尝试加载: 当前路径={}, 历史路径={}",
             connTagsPath_.string(), legacyPath.string());
  }
  return legacyStatus;
}

std::filesystem::path DataCenterConnTagsStore::connTagsPath() const {
  return connTagsPath_;
}

std::filesystem::path DataCenterConnTagsStore::backupPath() const {
  detail::ProtoFileStore<DataCenterProto::ConnTagsConfig> store(connTagsPath_, validateConnTagsConfig);
  return store.backupPath();
}

std::filesystem::path DataCenterConnTagsStore::tmpPath() const {
  detail::ProtoFileStore<DataCenterProto::ConnTagsConfig> store(connTagsPath_, validateConnTagsConfig);
  return store.tmpPath();
}
}  // namespace DataCenter
