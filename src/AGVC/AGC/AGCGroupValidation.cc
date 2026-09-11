#include "AGCGroupValidation.h"

#include <format>
#include <cmath>
#include <string>
#include <unordered_set>
#include <utility>

#include "AGCDefaultPoints.h"
#include "AGCNumeric.hpp"

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

grpc::Status validateSignalDecimal(const AGCProto::SignalSpec &signal,
                                   std::string_view fieldName) {
  auto scale = numeric::Scale(signal);
  if (!scale.has_value()) {
    return makeInvalid(std::format("{}.scale_decimal 非法: {}", fieldName,
                                   mskdsp::numeric::DecimalErrorMessage(scale.error())));
  }
  auto offset = numeric::Offset(signal);
  if (!offset.has_value()) {
    return makeInvalid(std::format("{}.offset_decimal 非法: {}", fieldName,
                                   mskdsp::numeric::DecimalErrorMessage(offset.error())));
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
  status = validateSignalDecimal(config.p_cmd().signal(), "p_cmd.signal");
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

  const auto mode = config.control_mode();
  if (mode != AGCProto::CONTROL_MODE_UNSPECIFIED && mode != AGCProto::CONTROL_MODE_PI_EVENT &&
      mode != AGCProto::CONTROL_MODE_DIRECT_CYCLIC) {
    return makeInvalid("control_mode 取值无效");
  }
  const auto calc = config.calculation_execution_period_seconds();
  const auto command = config.command_control_period_seconds();
  if (calc != 0.0 && (!std::isfinite(calc) || calc < 1.0 || calc > 15.0)) {
    return makeInvalid("calculation_execution_period_seconds 必须在 1～15 秒范围内");
  }
  if (command != 0.0 && (!std::isfinite(command) || command < 4.0 || command > 30.0)) {
    return makeInvalid("command_control_period_seconds 必须在 4～30 秒范围内");
  }
  if (mode == AGCProto::CONTROL_MODE_DIRECT_CYCLIC) {
    if (!std::isfinite(calc) || calc < 1.0 || calc > 15.0) {
      return makeInvalid("周期直分配模式 calculation_execution_period_seconds 必须在 1～15 秒范围内");
    }
    if (!std::isfinite(command) || command < 4.0 || command > 30.0) {
      return makeInvalid("周期直分配模式 command_control_period_seconds 必须在 4～30 秒范围内");
    }
  }

  if (config.has_outputs()) {
    const auto& outputs = config.outputs();
    if (outputs.has_p_total_meas()) {
      status = validateNoReservedDefaultTag(outputs.p_total_meas().tag(), "outputs.p_total_meas.tag");
      if (!status.ok()) {
        return status;
      }
      status = validateSignalDecimal(outputs.p_total_meas(), "outputs.p_total_meas");
      if (!status.ok()) {
        return status;
      }
    }
    if (outputs.has_p_total_target()) {
      status = validateNoReservedDefaultTag(outputs.p_total_target().tag(), "outputs.p_total_target.tag");
      if (!status.ok()) {
        return status;
      }
      status = validateSignalDecimal(outputs.p_total_target(), "outputs.p_total_target");
      if (!status.ok()) {
        return status;
      }
    }
    if (outputs.has_p_total_error()) {
      status = validateNoReservedDefaultTag(outputs.p_total_error().tag(), "outputs.p_total_error.tag");
      if (!status.ok()) {
        return status;
      }
      status = validateSignalDecimal(outputs.p_total_error(), "outputs.p_total_error");
      if (!status.ok()) {
        return status;
      }
    }
  }

  std::unordered_set<std::string> memberNames;
  memberNames.reserve(static_cast<size_t>(config.members_size()));
  numeric::Decimal installedCapacityKw;
  numeric::Decimal allocationWeightTotal;
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
    auto capacity = numeric::Capacity(m);
    if (!capacity.has_value() || *capacity <= numeric::Zero()) {
      return makeInvalid(std::format("成员 {} 的 capacity_kw 必须是大于 0 的有限数值", m.member_name()));
    }
    auto nextCapacity = installedCapacityKw.Add(*capacity);
    if (!nextCapacity.has_value()) {
      return makeInvalid("所有成员 capacity_kw 之和必须是有限数值");
    }
    installedCapacityKw = *nextCapacity;
    auto weight = numeric::Weight(m);
    auto minimum = numeric::Minimum(m);
    auto maximum = numeric::Maximum(m);
    if (!weight.has_value() || !minimum.has_value() || !maximum.has_value()) {
      return makeInvalid(std::format("成员 {} 的 weight/min_kw/max_kw 十进制配置非法", m.member_name()));
    }
    const auto effectiveMaximum =
        maximum->IsZero() || *maximum > *capacity ? *capacity : *maximum;
    if (*minimum > effectiveMaximum) {
      return makeInvalid(std::format(
          "成员 {} 的 min_kw 不能大于 capacity_kw 与 max_kw 共同确定的有效上限",
          m.member_name()));
    }
    if (m.controllable()) {
      const auto effectiveWeight =
          *weight > numeric::Zero() ? *weight : *capacity;
      auto nextWeight = allocationWeightTotal.Add(effectiveWeight);
      if (!nextWeight.has_value()) {
        return makeInvalid("可控成员的有效分配权重之和超出 Decimal20 范围");
      }
      allocationWeightTotal = *nextWeight;
    }
    status = validateNoReservedDefaultTag(m.p_meas().tag(), std::format("members[{}].p_meas.tag", m.member_name()));
    if (!status.ok()) {
      return status;
    }
    status = validateSignalDecimal(m.p_meas(), std::format("members[{}].p_meas", m.member_name()));
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
      status = validateSignalDecimal(
          m.p_set().signal(), std::format("members[{}].p_set.signal", m.member_name()));
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
