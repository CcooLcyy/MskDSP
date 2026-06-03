#include "CalcGroupStore.h"

#include <utility>

#include "CalcValidation.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace Calc {

GroupStore::GroupStore(std::filesystem::path configDbPath) :
  configDbPath_(std::move(configDbPath)) {}

grpc::Status GroupStore::Save(const CalcProto::GroupsConfig &config) {
  mskdsp::detail::ProtoSqliteStore<CalcProto::GroupsConfig> store(
      configDbPath_, "Calc", "groups", "CalcProto.GroupsConfig", ValidateGroupsConfig);
  return store.Save(config);
}

grpc::Status GroupStore::Load(CalcProto::GroupsConfig *out) {
  mskdsp::detail::ProtoSqliteStore<CalcProto::GroupsConfig> store(
      configDbPath_, "Calc", "groups", "CalcProto.GroupsConfig", ValidateGroupsConfig);
  return store.Load(out);
}

std::filesystem::path GroupStore::databasePath() const {
  return configDbPath_;
}

}  // namespace Calc
