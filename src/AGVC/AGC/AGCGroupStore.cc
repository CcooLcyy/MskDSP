#include "AGCGroupStore.h"

#include <utility>

#include "AGCGroupValidation.h"
#include "mskdsp/detail/ProtoFileStore.hpp"

namespace AGC {

AGCGroupStore::AGCGroupStore(std::filesystem::path groupsPath) :
  groupsPath_(std::move(groupsPath)) {}

grpc::Status AGCGroupStore::Save(const AGCProto::GroupsConfig& config) {
  mskdsp::detail::ProtoFileStore<AGCProto::GroupsConfig> store(groupsPath_,
                                                               ValidateGroupsConfig);
  return store.Save(config);
}

grpc::Status AGCGroupStore::Load(AGCProto::GroupsConfig* out) {
  mskdsp::detail::ProtoFileStore<AGCProto::GroupsConfig> store(groupsPath_,
                                                               ValidateGroupsConfig);
  return store.Load(out);
}

std::filesystem::path AGCGroupStore::groupsPath() const {
  return groupsPath_;
}

std::filesystem::path AGCGroupStore::backupPath() const {
  mskdsp::detail::ProtoFileStore<AGCProto::GroupsConfig> store(groupsPath_,
                                                               ValidateGroupsConfig);
  return store.backupPath();
}

std::filesystem::path AGCGroupStore::tmpPath() const {
  mskdsp::detail::ProtoFileStore<AGCProto::GroupsConfig> store(groupsPath_,
                                                               ValidateGroupsConfig);
  return store.tmpPath();
}

}  // namespace AGC
