#include "AGCControlProfileStore.h"

#include <cmath>
#include <format>
#include <string>
#include <unordered_set>
#include <utility>

#include "Logger.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace AGC {
namespace {
grpc::Status makeInvalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

bool finiteNonNegative(double value) {
  return std::isfinite(value) && value >= 0.0;
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
      if (!std::isfinite(member.up_p_gain()) || member.up_p_gain() < 0.0 ||
          !std::isfinite(member.up_i_gain()) || member.up_i_gain() < 0.0 ||
          !std::isfinite(member.down_p_gain()) || member.down_p_gain() < 0.0 ||
          !std::isfinite(member.down_i_gain()) || member.down_i_gain() < 0.0) {
        return makeInvalid(std::format("控制参数系数必须是非负有限数值: group_name={}, member_name={}",
                                       profile.group_name(), member.member_name()));
      }
      if (!std::isfinite(member.up_bias_kw()) || !std::isfinite(member.down_bias_kw()) ||
          !finiteNonNegative(member.integral_limit_kw()) ||
          !finiteNonNegative(member.max_step_kw()) ||
          !finiteNonNegative(member.max_ramp_kw_per_s())) {
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
