#include "AVCGroupValidation.h"

#include <format>
#include <string>
#include <unordered_set>
#include <utility>

#include "AVCDefaultPoints.h"

namespace AVC {
namespace {

grpc::Status makeInvalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

grpc::Status validateNoReservedDefaultTag(const std::string& tag, std::string_view fieldName) {
  if (!tag.empty() && IsReservedDefaultPointTag(tag)) {
    return makeInvalid(std::format("{} 不能使用 AVC 默认点保留 tag: {}", fieldName, tag));
  }
  return grpc::Status::OK;
}

}  // namespace

grpc::Status ValidateGroupConfig(const AVCProto::GroupConfig& config) {
  if (config.group_name().empty()) {
    return makeInvalid("group_name 不能为空");
  }
  if (!config.has_voltage_meas() || config.voltage_meas().tag().empty()) {
    return makeInvalid("voltage_meas.tag 不能为空");
  }
  auto status = validateNoReservedDefaultTag(config.voltage_meas().tag(), "voltage_meas.tag");
  if (!status.ok()) {
    return status;
  }

  switch (config.command_case()) {
  case AVCProto::GroupConfig::kVoltageCmd:
    if (config.voltage_cmd().tag().empty()) {
      return makeInvalid("voltage_cmd.tag 不能为空");
    }
    status = validateNoReservedDefaultTag(config.voltage_cmd().tag(), "voltage_cmd.tag");
    if (!status.ok()) {
      return status;
    }
    if (config.voltage_control().kp() <= 0.0) {
      return makeInvalid("目标电压模式要求 voltage_control.kp > 0");
    }
    if (config.voltage_control().deadband() < 0.0) {
      return makeInvalid("voltage_control.deadband 不能小于 0");
    }
    break;
  case AVCProto::GroupConfig::kQTotalCmd:
    if (!config.q_total_cmd().has_signal() || config.q_total_cmd().signal().tag().empty()) {
      return makeInvalid("q_total_cmd.signal.tag 不能为空");
    }
    status = validateNoReservedDefaultTag(config.q_total_cmd().signal().tag(), "q_total_cmd.signal.tag");
    if (!status.ok()) {
      return status;
    }
    if (config.q_total_cmd().mode() == AVCProto::VALUE_MODE_DELTA &&
        config.q_total_cmd().delta_base() == AVCProto::DELTA_BASE_BASE_TAG) {
      status = validateNoReservedDefaultTag(config.q_total_cmd().base_tag(), "q_total_cmd.base_tag");
      if (!status.ok()) {
        return status;
      }
    }
    if (config.voltage_control().deadband() < 0.0) {
      return makeInvalid("voltage_control.deadband 不能小于 0");
    }
    break;
  case AVCProto::GroupConfig::COMMAND_NOT_SET:
  default:
    return makeInvalid("必须配置 voltage_cmd 或 q_total_cmd");
  }

  if (config.members_size() <= 0) {
    return makeInvalid("members 不能为空");
  }

  std::unordered_set<std::string> memberNames;
  memberNames.reserve(static_cast<size_t>(config.members_size()));
  for (const auto& member : config.members()) {
    if (member.member_name().empty()) {
      return makeInvalid("members.member_name 不能为空");
    }
    if (!memberNames.emplace(member.member_name()).second) {
      return makeInvalid(std::format("member_name 重复: {}", member.member_name()));
    }
    if (!member.has_q_meas() || member.q_meas().tag().empty()) {
      return makeInvalid(std::format("members[{}].q_meas.tag 不能为空", member.member_name()));
    }
    status = validateNoReservedDefaultTag(member.q_meas().tag(), std::format("members[{}].q_meas.tag", member.member_name()));
    if (!status.ok()) {
      return status;
    }
    if (member.q_min_kvar() > member.q_max_kvar()) {
      return makeInvalid(std::format("members[{}].q_min_kvar 不能大于 q_max_kvar", member.member_name()));
    }
    if (member.controllable()) {
      if (!member.has_q_set() || !member.q_set().has_signal() || member.q_set().signal().tag().empty()) {
        return makeInvalid(std::format("members[{}].q_set.signal.tag 不能为空（可控成员）", member.member_name()));
      }
    }
    if (member.has_q_set() && member.q_set().has_signal()) {
      status = validateNoReservedDefaultTag(
          member.q_set().signal().tag(), std::format("members[{}].q_set.signal.tag", member.member_name()));
      if (!status.ok()) {
        return status;
      }
      if (member.q_set().mode() == AVCProto::VALUE_MODE_DELTA &&
          member.q_set().delta_base() == AVCProto::DELTA_BASE_BASE_TAG) {
        status = validateNoReservedDefaultTag(
            member.q_set().base_tag(), std::format("members[{}].q_set.base_tag", member.member_name()));
        if (!status.ok()) {
          return status;
        }
      }
    }
  }
  return grpc::Status::OK;
}

grpc::Status ValidateGroupsConfig(const AVCProto::GroupsConfig& config) {
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

}  // namespace AVC
