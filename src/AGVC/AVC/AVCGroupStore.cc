#include "AVCGroupStore.h"

#include <utility>

#include "AVCGroupValidation.h"
#include "Logger.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace AVC {
namespace {
void logConfigStoreTrace(const std::string& message) {
  LOG_INFO("{}", message);
}
}  // namespace

AVCGroupStore::AVCGroupStore(std::filesystem::path configDbPath) :
  configDbPath_(std::move(configDbPath)) {}

grpc::Status AVCGroupStore::Save(const AVCProto::GroupsConfig& config) {
  mskdsp::detail::ProtoSqliteStore<AVCProto::GroupsConfig> store(
      configDbPath_, "AVC", "groups", "AVCProto.GroupsConfig", ValidateGroupsConfig, logConfigStoreTrace);
  return store.Save(config);
}

grpc::Status AVCGroupStore::Load(AVCProto::GroupsConfig* out) {
  mskdsp::detail::ProtoSqliteStore<AVCProto::GroupsConfig> store(
      configDbPath_, "AVC", "groups", "AVCProto.GroupsConfig", ValidateGroupsConfig, logConfigStoreTrace);
  return store.Load(out);
}

std::filesystem::path AVCGroupStore::databasePath() const {
  return configDbPath_;
}

}  // namespace AVC
