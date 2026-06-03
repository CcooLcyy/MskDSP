#include "AVCGroupStore.h"

#include <utility>

#include "AVCGroupValidation.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace AVC {

AVCGroupStore::AVCGroupStore(std::filesystem::path configDbPath) :
  configDbPath_(std::move(configDbPath)) {}

grpc::Status AVCGroupStore::Save(const AVCProto::GroupsConfig& config) {
  mskdsp::detail::ProtoSqliteStore<AVCProto::GroupsConfig> store(
      configDbPath_, "AVC", "groups", "AVCProto.GroupsConfig", ValidateGroupsConfig);
  return store.Save(config);
}

grpc::Status AVCGroupStore::Load(AVCProto::GroupsConfig* out) {
  mskdsp::detail::ProtoSqliteStore<AVCProto::GroupsConfig> store(
      configDbPath_, "AVC", "groups", "AVCProto.GroupsConfig", ValidateGroupsConfig);
  return store.Load(out);
}

std::filesystem::path AVCGroupStore::databasePath() const {
  return configDbPath_;
}

}  // namespace AVC
