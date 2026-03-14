#include "AGCGroupStore.h"

#include <utility>

#include "AGCGroupValidation.h"
#include "detail/ProtoFileStore.hpp"

namespace AGC {

AGCGroupStore::AGCGroupStore(std::filesystem::path groupsPath) :
  groupsPath_(std::move(groupsPath)) {}

grpc::Status AGCGroupStore::Save(const AGCProto::GroupsConfig& config) {
  detail::ProtoFileStore<AGCProto::GroupsConfig> store(groupsPath_, ValidateGroupsConfig);
  return store.Save(config);
}

grpc::Status AGCGroupStore::Load(AGCProto::GroupsConfig* out) {
  detail::ProtoFileStore<AGCProto::GroupsConfig> store(groupsPath_, ValidateGroupsConfig);
  return store.Load(out);
}

std::filesystem::path AGCGroupStore::groupsPath() const {
  return groupsPath_;
}

std::filesystem::path AGCGroupStore::backupPath() const {
  detail::ProtoFileStore<AGCProto::GroupsConfig> store(groupsPath_, ValidateGroupsConfig);
  return store.backupPath();
}

std::filesystem::path AGCGroupStore::tmpPath() const {
  detail::ProtoFileStore<AGCProto::GroupsConfig> store(groupsPath_, ValidateGroupsConfig);
  return store.tmpPath();
}

}  // namespace AGC
