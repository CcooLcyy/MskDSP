#include "IEC61850ConfigStore.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "IEC61850ConfigValidation.h"
#include "IEC61850ModelSelection.h"
#include "Logger.h"
#include "mskdsp/ConfigDatabase.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace IEC61850 {
namespace {

constexpr auto kModuleName = "IEC61850";
constexpr auto kConfigKey = "config";
constexpr auto kProtoType = "IEC61850Proto.PersistedConfig";

void LogStoreTrace(const std::string& message) {
  LOG_INFO("IEC61850配置存储: {}", message);
}

IEC61850Proto::PersistedConfig Normalize(
    IEC61850Proto::PersistedConfig config) {
  if (config.schema_version() == 0) {
    config.set_schema_version(1);
  }
  std::size_t normalizedObjects = 0;
  for (auto& model : *config.mutable_models()) {
    for (auto& ied : *model.mutable_ieds()) {
      normalizedObjects += NormalizeSingleServerAccessPointOwnership(&ied);
    }
  }
  if (normalizedObjects > 0) {
    LOG_INFO("IEC61850配置存储已补齐旧模型的AccessPoint归属: 对象数量={}",
             normalizedObjects);
  }
  return config;
}

grpc::Status Validate(const IEC61850Proto::PersistedConfig& config) {
  std::vector<IEC61850Proto::ValidationIssue> issues;
  return ValidatePersistedConfig(config, &issues);
}

mskdsp::detail::ProtoSqliteStore<IEC61850Proto::PersistedConfig> MakeStore(
    const std::filesystem::path& databasePath) {
  return {databasePath, kModuleName, kConfigKey, kProtoType, Validate,
          LogStoreTrace, Normalize};
}

}  // namespace

ConfigStore::ConfigStore(std::filesystem::path databasePath) :
  databasePath_(std::move(databasePath)) {}

grpc::Status ConfigStore::Save(
    const IEC61850Proto::PersistedConfig& config) const {
  return MakeStore(databasePath_).Save(config);
}

grpc::Status ConfigStore::Load(IEC61850Proto::PersistedConfig* out) const {
  auto status = MakeStore(databasePath_).Load(out);
  if (status.ok() && out != nullptr && out->schema_version() == 0) {
    out->set_schema_version(1);
  }
  return status;
}

grpc::Status ConfigStore::Clear() const {
  mskdsp::ConfigDatabase database(databasePath_);
  return database.DeleteBlobs(kModuleName, {kConfigKey}, LogStoreTrace);
}

const std::filesystem::path& ConfigStore::databasePath() const {
  return databasePath_;
}

}  // namespace IEC61850
