#include "AGCGroupValidation.h"

#include <format>
#include <string>
#include <unordered_set>
#include <utility>

namespace AGC {
namespace {

grpc::Status makeInvalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

}  // namespace

grpc::Status ValidateGroupConfig(const AGCProto::GroupConfig& config) {
  if (config.group_name().empty()) {
    return makeInvalid("group_name 不能为空");
  }
  if (!config.has_p_cmd() || !config.p_cmd().has_signal()) {
    return makeInvalid("p_cmd.signal 不能为空");
  }
  if (config.p_cmd().signal().tag().empty()) {
    return makeInvalid("p_cmd.signal.tag 不能为空");
  }
  if (config.members_size() <= 0) {
    return makeInvalid("members 不能为空");
  }

  std::unordered_set<std::string> memberNames;
  memberNames.reserve(static_cast<size_t>(config.members_size()));
  for (const auto& m : config.members()) {
    if (m.member_name().empty()) {
      return makeInvalid("members.member_name 不能为空");
    }
    if (!memberNames.emplace(m.member_name()).second) {
      return makeInvalid(std::format("member_name 重复: {}", m.member_name()));
    }
    if (!m.has_p_meas() || m.p_meas().tag().empty()) {
      return makeInvalid(std::format("members[{}].p_meas.tag 不能为空", m.member_name()));
    }
    if (m.controllable()) {
      if (!m.has_p_set() || !m.p_set().has_signal() || m.p_set().signal().tag().empty()) {
        return makeInvalid(std::format("members[{}].p_set.signal.tag 不能为空（可控成员）", m.member_name()));
      }
    }
  }
  return grpc::Status::OK;
}

grpc::Status ValidateGroupsConfig(const AGCProto::GroupsConfig& config) {
  std::unordered_set<std::string> groupNames;
  if (config.persisted_groups_size() > 0) {
    groupNames.reserve(static_cast<size_t>(config.persisted_groups_size()));
    for (const auto& persisted : config.persisted_groups()) {
      if (!persisted.has_config()) {
        return makeInvalid("persisted_groups.config 不能为空");
      }
      auto status = ValidateGroupConfig(persisted.config());
      if (!status.ok()) {
        return status;
      }
      if (!groupNames.emplace(persisted.config().group_name()).second) {
        return makeInvalid(std::format("persisted_groups 包含重复的 group_name: {}", persisted.config().group_name()));
      }
    }
    return grpc::Status::OK;
  }

  groupNames.reserve(static_cast<size_t>(config.groups_size()));
  for (const auto& group : config.groups()) {
    auto status = ValidateGroupConfig(group);
    if (!status.ok()) {
      return status;
    }
    if (!groupNames.emplace(group.group_name()).second) {
      return makeInvalid(std::format("groups 包含重复的 group_name: {}", group.group_name()));
    }
  }
  return grpc::Status::OK;
}

}  // namespace AGC
