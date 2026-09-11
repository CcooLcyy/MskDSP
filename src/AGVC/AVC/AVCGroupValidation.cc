#include "AVCGroupValidation.h"

#include <format>
#include <cmath>
#include <string>
#include <unordered_set>
#include <utility>

#include "AVCDefaultPoints.h"
#include "AVCNumeric.hpp"

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

grpc::Status validateSignalDecimal(const AVCProto::SignalSpec& signal,
                                   std::string_view fieldName) {
  auto scale = numeric::Scale(signal);
  if (!scale.has_value()) {
    return makeInvalid(std::format("{}.scale_decimal 非法: {}", fieldName,
                                   mskdsp::numeric::DecimalErrorMessage(
                                       scale.error())));
  }
  auto offset = numeric::Offset(signal);
  if (!offset.has_value()) {
    return makeInvalid(std::format("{}.offset_decimal 非法: {}", fieldName,
                                   mskdsp::numeric::DecimalErrorMessage(
                                       offset.error())));
  }
  return grpc::Status::OK;
}

}  // namespace

grpc::Status ValidateGroupConfig(const AVCProto::GroupConfig& config) {
  if (config.group_name().empty()) {
    return makeInvalid("group_name 不能为空");
  }
  const auto mode = config.control_mode();
  if (mode != AVCProto::CONTROL_MODE_UNSPECIFIED && mode != AVCProto::CONTROL_MODE_PI_EVENT &&
      mode != AVCProto::CONTROL_MODE_DIRECT_CYCLIC) {
    return makeInvalid("control_mode 无效");
  }
  // 周期字段如有配置始终校验；未配置的 PI_EVENT/UNSPECIFIED 保持旧行为。
  if (config.calculation_execution_period_seconds() != 0.0 &&
      (!std::isfinite(config.calculation_execution_period_seconds()) ||
       config.calculation_execution_period_seconds() < 1.0 || config.calculation_execution_period_seconds() > 15.0)) {
    return makeInvalid("calculation_execution_period_seconds 必须在 1~15 秒范围内");
  }
  if (config.command_control_period_seconds() != 0.0 &&
      (!std::isfinite(config.command_control_period_seconds()) ||
       config.command_control_period_seconds() < 4.0 || config.command_control_period_seconds() > 30.0)) {
    return makeInvalid("command_control_period_seconds 必须在 4~30 秒范围内");
  }
  if (mode == AVCProto::CONTROL_MODE_DIRECT_CYCLIC) {
    if (config.calculation_execution_period_seconds() < 1.0 || config.calculation_execution_period_seconds() > 15.0) {
      return makeInvalid("DIRECT_CYCLIC 模式要求 calculation_execution_period_seconds 在 1~15 秒范围内");
    }
    if (config.command_control_period_seconds() < 4.0 || config.command_control_period_seconds() > 30.0) {
      return makeInvalid("DIRECT_CYCLIC 模式要求 command_control_period_seconds 在 4~30 秒范围内");
    }
    if (config.command_case() != AVCProto::GroupConfig::kQTotalCmd) {
      return makeInvalid("AVC DIRECT_CYCLIC 模式必须使用 q_total_cmd，总无功目标电压调节仍使用 PI_EVENT 模式");
    }
  }
  if (!config.has_voltage_meas() || config.voltage_meas().tag().empty()) {
    return makeInvalid("voltage_meas.tag 不能为空");
  }
  auto status = validateNoReservedDefaultTag(config.voltage_meas().tag(), "voltage_meas.tag");
  if (!status.ok()) {
    return status;
  }
  status = validateSignalDecimal(config.voltage_meas(), "voltage_meas");
  if (!status.ok()) {
    return status;
  }

  auto deadband = numeric::Deadband(config.voltage_control());
  if (!deadband.has_value()) {
    return makeInvalid(std::format(
        "voltage_control.deadband_decimal 非法: {}",
        mskdsp::numeric::DecimalErrorMessage(deadband.error())));
  }
  auto kp = numeric::Kp(config.voltage_control());
  if (!kp.has_value()) {
    return makeInvalid(std::format(
        "voltage_control.kp_decimal 非法: {}",
        mskdsp::numeric::DecimalErrorMessage(kp.error())));
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
    status = validateSignalDecimal(config.voltage_cmd(), "voltage_cmd");
    if (!status.ok()) {
      return status;
    }
    if (*kp <= numeric::Zero()) {
      return makeInvalid("目标电压模式要求 voltage_control.kp > 0");
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
    status = validateSignalDecimal(config.q_total_cmd().signal(),
                                   "q_total_cmd.signal");
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
    auto weight = numeric::Weight(member);
    auto minimum = numeric::Minimum(member);
    auto maximum = numeric::Maximum(member);
    if (!weight.has_value() || !minimum.has_value() || !maximum.has_value()) {
      return makeInvalid(std::format(
          "members[{}] 的 weight/q_min_kvar/q_max_kvar 十进制配置非法",
          member.member_name()));
    }
    if (*minimum > *maximum) {
      return makeInvalid(std::format("members[{}].q_min_kvar 不能大于 q_max_kvar", member.member_name()));
    }
    status = validateSignalDecimal(
        member.q_meas(),
        std::format("members[{}].q_meas", member.member_name()));
    if (!status.ok()) {
      return status;
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
      status = validateSignalDecimal(
          member.q_set().signal(),
          std::format("members[{}].q_set.signal", member.member_name()));
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
