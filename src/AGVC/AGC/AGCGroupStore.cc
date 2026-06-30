#include "AGCGroupStore.h"

#include <utility>

#include "AGCGroupValidation.h"
#include "Logger.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace AGC {
namespace {
void logConfigStoreTrace(const std::string& message) {
  LOG_INFO("{}", message);
}
}  // namespace

AGCGroupStore::AGCGroupStore(std::filesystem::path configDbPath) :
  configDbPath_(std::move(configDbPath)) {}

grpc::Status AGCGroupStore::Save(const AGCProto::GroupsConfig& config) {
  mskdsp::detail::ProtoSqliteStore<AGCProto::GroupsConfig> store(
      configDbPath_, "AGC", "groups", "AGCProto.GroupsConfig", ValidateGroupsConfig, logConfigStoreTrace);
  return store.Save(config);
}

grpc::Status AGCGroupStore::Load(AGCProto::GroupsConfig* out) {
  mskdsp::detail::ProtoSqliteStore<AGCProto::GroupsConfig> store(
      configDbPath_, "AGC", "groups", "AGCProto.GroupsConfig", ValidateGroupsConfig, logConfigStoreTrace);
  return store.Load(out);
}

std::filesystem::path AGCGroupStore::databasePath() const {
  return configDbPath_;
}

}  // namespace AGC
