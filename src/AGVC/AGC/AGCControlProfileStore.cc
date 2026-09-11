#include "AGCControlProfileStore.h"

#include <cmath>
#include <format>
#include <string>
#include <unordered_set>
#include <utility>

#include "Logger.h"
#include "AGCNumeric.hpp"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace AGC {
namespace {
grpc::Status makeInvalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

void logConfigStoreTrace(const std::string& message) {
  LOG_INFO("{}", message);
}
}  // namespace

grpc::Status ValidateControlProfilesConfig(const AGCProto::ControlProfilesConfig& config) {
  std::unordered_set<std::string> groupNames;
  for (const auto& profile : config.profiles()) {
    if (profile.group_name().empty()) {
      return makeInvalid("控制参数 group_name 不能为空");
    }
    if (!groupNames.emplace(profile.group_name()).second) {
      return makeInvalid(std::format("控制参数 group_name 重复: {}", profile.group_name()));
    }

    std::unordered_set<std::string> memberNames;
    for (const auto& member : profile.members()) {
      if (member.member_name().empty()) {
        return makeInvalid(std::format("控制参数 {} 的 member_name 不能为空", profile.group_name()));
      }
      if (!memberNames.emplace(member.member_name()).second) {
        return makeInvalid(std::format("控制参数成员重复: group_name={}, member_name={}", profile.group_name(), member.member_name()));
      }
      auto upP = numeric::UpPGain(member);
      auto upI = numeric::UpIGain(member);
      auto downP = numeric::DownPGain(member);
      auto downI = numeric::DownIGain(member);
      if (!upP.has_value() || *upP < numeric::Zero() ||
          !upI.has_value() || *upI < numeric::Zero() ||
          !downP.has_value() || *downP < numeric::Zero() ||
          !downI.has_value() || *downI < numeric::Zero()) {
        return makeInvalid(std::format("控制参数系数必须是非负有限数值: group_name={}, member_name={}",
                                       profile.group_name(), member.member_name()));
      }
      auto upBias = numeric::UpBias(member);
      auto downBias = numeric::DownBias(member);
      auto integralLimit = numeric::IntegralLimit(member);
      auto maximumStep = numeric::MaximumStep(member);
      auto maximumRamp = numeric::MaximumRamp(member);
      if (!upBias.has_value() || !downBias.has_value() ||
          !integralLimit.has_value() || *integralLimit < numeric::Zero() ||
          !maximumStep.has_value() || *maximumStep < numeric::Zero() ||
          !maximumRamp.has_value() || *maximumRamp < numeric::Zero()) {
        return makeInvalid(std::format("控制参数范围非法: group_name={}, member_name={}",
                                       profile.group_name(), member.member_name()));
      }
    }
  }
  return grpc::Status::OK;
}

AGCControlProfileStore::AGCControlProfileStore(std::filesystem::path configDbPath) :
  configDbPath_(std::move(configDbPath)) {}

grpc::Status AGCControlProfileStore::Save(const AGCProto::ControlProfilesConfig& config) {
  mskdsp::detail::ProtoSqliteStore<AGCProto::ControlProfilesConfig> store(
      configDbPath_, "AGC", "control_profiles", "AGCProto.ControlProfilesConfig", ValidateControlProfilesConfig, logConfigStoreTrace);
  return store.Save(config);
}

grpc::Status AGCControlProfileStore::Load(AGCProto::ControlProfilesConfig* out) {
  mskdsp::detail::ProtoSqliteStore<AGCProto::ControlProfilesConfig> store(
      configDbPath_, "AGC", "control_profiles", "AGCProto.ControlProfilesConfig", ValidateControlProfilesConfig, logConfigStoreTrace);
  return store.Load(out);
}

const std::filesystem::path& AGCControlProfileStore::databasePath() const {
  return configDbPath_;
}

}  // namespace AGC
