#include "CalcGroupStore.h"

#include <utility>

#include "CalcValidation.h"
#include "mskdsp/detail/ProtoFileStore.hpp"

namespace Calc {

GroupStore::GroupStore(std::filesystem::path groupsPath) :
  groupsPath_(std::move(groupsPath)) {}

grpc::Status GroupStore::Save(const CalcProto::GroupsConfig &config) {
  mskdsp::detail::ProtoFileStore<CalcProto::GroupsConfig> store(groupsPath_, ValidateGroupsConfig);
  return store.Save(config);
}

grpc::Status GroupStore::Load(CalcProto::GroupsConfig *out) {
  mskdsp::detail::ProtoFileStore<CalcProto::GroupsConfig> store(groupsPath_, ValidateGroupsConfig);
  return store.Load(out);
}

std::filesystem::path GroupStore::groupsPath() const {
  return groupsPath_;
}

std::filesystem::path GroupStore::backupPath() const {
  mskdsp::detail::ProtoFileStore<CalcProto::GroupsConfig> store(groupsPath_, ValidateGroupsConfig);
  return store.backupPath();
}

std::filesystem::path GroupStore::tmpPath() const {
  mskdsp::detail::ProtoFileStore<CalcProto::GroupsConfig> store(groupsPath_, ValidateGroupsConfig);
  return store.tmpPath();
}

}  // namespace Calc
