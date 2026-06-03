#include "AGCGroupStore.h"

#include <utility>

#include "AGCGroupValidation.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace AGC {

AGCGroupStore::AGCGroupStore(std::filesystem::path configDbPath) :
  configDbPath_(std::move(configDbPath)) {}

grpc::Status AGCGroupStore::Save(const AGCProto::GroupsConfig& config) {
  mskdsp::detail::ProtoSqliteStore<AGCProto::GroupsConfig> store(
      configDbPath_, "AGC", "groups", "AGCProto.GroupsConfig", ValidateGroupsConfig);
  return store.Save(config);
}

grpc::Status AGCGroupStore::Load(AGCProto::GroupsConfig* out) {
  mskdsp::detail::ProtoSqliteStore<AGCProto::GroupsConfig> store(
      configDbPath_, "AGC", "groups", "AGCProto.GroupsConfig", ValidateGroupsConfig);
  return store.Load(out);
}

std::filesystem::path AGCGroupStore::databasePath() const {
  return configDbPath_;
}

}  // namespace AGC
