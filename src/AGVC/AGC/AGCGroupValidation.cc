#include "AGCGroupValidation.h"

#include <format>
#include <cmath>
#include <string>
#include <unordered_set>
#include <utility>

#include "AGCDefaultPoints.h"

namespace AGC {
namespace {

grpc::Status makeInvalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

grpc::Status validateNoReservedDefaultTag(const std::string& tag, std::string_view fieldName) {
  if (!tag.empty() && IsReservedDefaultPointTag(tag)) {
    return makeInvalid(std::format("{} 不能使用 AGC 默认点保留 tag: {}", fieldName, tag));
  }
  return grpc::Status::OK;
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
  auto status = validateNoReservedDefaultTag(config.p_cmd().signal().tag(), "p_cmd.signal.tag");
  if (!status.ok()) {
    return status;
  }
  if (config.p_cmd().mode() == AGCProto::VALUE_MODE_DELTA && config.p_cmd().delta_base() == AGCProto::DELTA_BASE_BASE_TAG) {
    status = validateNoReservedDefaultTag(config.p_cmd().base_tag(), "p_cmd.base_tag");
    if (!status.ok()) {
      return status;
    }
  }
  if (config.members_size() <= 0) {
    return makeInvalid("members 不能为空");
  }

  if (config.has_outputs()) {
    const auto& outputs = config.outputs();
    if (outputs.has_p_total_meas()) {
      status = validateNoReservedDefaultTag(outputs.p_total_meas().tag(), "outputs.p_total_meas.tag");
      if (!status.ok()) {
        return status;
      }
    }
    if (outputs.has_p_total_target()) {
      status = validateNoReservedDefaultTag(outputs.p_total_target().tag(), "outputs.p_total_target.tag");
      if (!status.ok()) {
        return status;
      }
    }
    if (outputs.has_p_total_error()) {
      status = validateNoReservedDefaultTag(outputs.p_total_error().tag(), "outputs.p_total_error.tag");
      if (!status.ok()) {
        return status;
      }
    }
  }

  std::unordered_set<std::string> memberNames;
  memberNames.reserve(static_cast<size_t>(config.members_size()));
  double installedCapacityKw = 0.0;
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
    if (!std::isfinite(m.capacity_kw()) || m.capacity_kw() <= 0.0) {
      return makeInvalid(std::format("成员 {} 的 capacity_kw 必须是大于 0 的有限数值", m.member_name()));
    }
    installedCapacityKw += m.capacity_kw();
    if (!std::isfinite(installedCapacityKw)) {
      return makeInvalid("所有成员 capacity_kw 之和必须是有限数值");
    }
    status = validateNoReservedDefaultTag(m.p_meas().tag(), std::format("members[{}].p_meas.tag", m.member_name()));
    if (!status.ok()) {
      return status;
    }
    if (m.controllable()) {
      if (!m.has_p_set() || !m.p_set().has_signal() || m.p_set().signal().tag().empty()) {
        return makeInvalid(std::format("members[{}].p_set.signal.tag 不能为空（可控成员）", m.member_name()));
      }
    }
    if (m.has_p_set() && m.p_set().has_signal()) {
      status = validateNoReservedDefaultTag(
          m.p_set().signal().tag(), std::format("members[{}].p_set.signal.tag", m.member_name()));
      if (!status.ok()) {
        return status;
      }
      if (m.p_set().mode() == AGCProto::VALUE_MODE_DELTA && m.p_set().delta_base() == AGCProto::DELTA_BASE_BASE_TAG) {
        status = validateNoReservedDefaultTag(m.p_set().base_tag(), std::format("members[{}].p_set.base_tag", m.member_name()));
        if (!status.ok()) {
          return status;
        }
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
