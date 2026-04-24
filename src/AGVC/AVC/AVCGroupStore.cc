#include "AVCGroupStore.h"

#include <utility>

#include "AVCGroupValidation.h"
#include "mskdsp/detail/ProtoFileStore.hpp"

namespace AVC {

AVCGroupStore::AVCGroupStore(std::filesystem::path groupsPath) :
  groupsPath_(std::move(groupsPath)) {}

grpc::Status AVCGroupStore::Save(const AVCProto::GroupsConfig& config) {
  mskdsp::detail::ProtoFileStore<AVCProto::GroupsConfig> store(groupsPath_,
                                                               ValidateGroupsConfig);
  return store.Save(config);
}

grpc::Status AVCGroupStore::Load(AVCProto::GroupsConfig* out) {
  mskdsp::detail::ProtoFileStore<AVCProto::GroupsConfig> store(groupsPath_,
                                                               ValidateGroupsConfig);
  return store.Load(out);
}

std::filesystem::path AVCGroupStore::groupsPath() const {
  return groupsPath_;
}

std::filesystem::path AVCGroupStore::backupPath() const {
  mskdsp::detail::ProtoFileStore<AVCProto::GroupsConfig> store(groupsPath_,
                                                               ValidateGroupsConfig);
  return store.backupPath();
}

std::filesystem::path AVCGroupStore::tmpPath() const {
  mskdsp::detail::ProtoFileStore<AVCProto::GroupsConfig> store(groupsPath_,
                                                               ValidateGroupsConfig);
  return store.tmpPath();
}

}  // namespace AVC
