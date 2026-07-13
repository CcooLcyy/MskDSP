#include "AVCControl.h"

#include <algorithm>
#include <cmath>

#include "Logger.h"

namespace AVC {
namespace {

double effectiveScale(const AVCProto::SignalSpec& s) {
  return (s.scale() == 0.0) ? 1.0 : s.scale();
}

double toPhysicalAbs(const AVCProto::SignalSpec& s, double raw) {
  const auto scale = effectiveScale(s);
  return raw * scale + s.offset();
}

double toPhysicalDelta(const AVCProto::SignalSpec& s, double rawDelta) {
  const auto scale = effectiveScale(s);
  return rawDelta * scale;
}

bool isVoltageMode(const AVCProto::GroupConfig& config) {
  return config.command_case() == AVCProto::GroupConfig::kVoltageCmd;
}

}  // namespace

std::optional<double> ComputeVoltageMeas(const AVCProto::GroupConfig& config, const ControlInput& input) {
  if (!input.hasVoltageMeasRaw) {
    return std::nullopt;
  }
  return toPhysicalAbs(config.voltage_meas(), input.voltageMeasRaw);
}

double ComputeTotalQMeasKvar(const AVCProto::GroupConfig& config, const ControlInput& input) {
  const auto memberCount = static_cast<size_t>(config.members_size());
  double totalQMeasKvar = 0.0;
  for (size_t i = 0; i < memberCount; ++i) {
    if (i < input.memberQMeasRaw.size() && i < input.hasMemberQMeasRaw.size() && input.hasMemberQMeasRaw[i]) {
      totalQMeasKvar += toPhysicalAbs(config.members(static_cast<int>(i)).q_meas(), input.memberQMeasRaw[i]);
    }
  }
  return totalQMeasKvar;
}

DefaultPointOutput ComputeDefaultPointOutput(const AVCProto::GroupConfig& config, const ControlInput& input) {
  DefaultPointOutput out;
  const auto memberCount = static_cast<size_t>(config.members_size());
  for (size_t i = 0; i < memberCount; ++i) {
    const auto& member = config.members(static_cast<int>(i));
    if (member.controllable()) {
      out.theoreticalLowerQKvar += member.q_min_kvar();
      out.theoreticalUpperQKvar += member.q_max_kvar();
      continue;
    }

    ++out.uncontrollableMemberCount;
    if (i < input.memberQMeasRaw.size() && i < input.hasMemberQMeasRaw.size() && input.hasMemberQMeasRaw[i]) {
      const auto measQKvar = toPhysicalAbs(member.q_meas(), input.memberQMeasRaw[i]);
      out.dynamicLowerQKvar += measQKvar;
      out.dynamicUpperQKvar += measQKvar;
      continue;
    }
    ++out.missingUncontrollableMemberCount;
  }

  out.dynamicLowerQKvar += out.theoreticalLowerQKvar;
  out.dynamicUpperQKvar += out.theoreticalUpperQKvar;
  out.dynamicQuality = (out.missingUncontrollableMemberCount == 0) ? DataCenterProto::QUALITY_GOOD
                                                                   : DataCenterProto::QUALITY_BAD;
  return out;
}

std::optional<ControlOutput> ComputeControlOutput(
    const AVCProto::GroupConfig& config,
    const ControlInput& input,
    const AGVC::WeightedStrategy& strategy) {
  const auto memberCount = static_cast<size_t>(config.members_size());
  if (memberCount == 0) {
    LOG_WARNING("AVC 控制计算跳过: group_name={}, 原因=成员列表为空", config.group_name());
    return std::nullopt;
  }

  ControlOutput out;
  out.totalQMeasKvar = ComputeTotalQMeasKvar(config, input);
  if (const auto voltageMeas = ComputeVoltageMeas(config, input)) {
    out.hasVoltageMeas = true;
    out.voltageMeas = *voltageMeas;
  }
  out.memberTargetQKvar.assign(memberCount, 0.0);
  out.memberPublish.assign(memberCount, false);
  out.memberPublishKvar.assign(memberCount, 0.0);

  std::vector<double> memberQMeasKvar(memberCount, 0.0);
  for (size_t i = 0; i < memberCount; ++i) {
    if (i < input.memberQMeasRaw.size() && i < input.hasMemberQMeasRaw.size() && input.hasMemberQMeasRaw[i]) {
      memberQMeasKvar[i] = toPhysicalAbs(config.members(static_cast<int>(i)).q_meas(), input.memberQMeasRaw[i]);
    }
  }

  double desiredTotalQKvar = 0.0;
  if (isVoltageMode(config)) {
    if (!input.hasVoltageCmdRaw) {
      LOG_DEBUG("AVC 控制计算跳过: group_name={}, 原因=尚未收到目标电压输入", config.group_name());
      return std::nullopt;
    }
    if (!out.hasVoltageMeas) {
      LOG_DEBUG("AVC 控制计算跳过: group_name={}, 原因=尚未收到主电压量测", config.group_name());
      return std::nullopt;
    }

    const auto targetVoltage = toPhysicalAbs(config.voltage_cmd(), input.voltageCmdRaw);
    const auto errorV = targetVoltage - out.voltageMeas;
    out.hasVoltageError = true;
    out.voltageError = errorV;
    if (std::fabs(errorV) <= config.voltage_control().deadband()) {
      desiredTotalQKvar = out.totalQMeasKvar;
    } else {
      desiredTotalQKvar = out.totalQMeasKvar + config.voltage_control().kp() * errorV;
    }
  } else {
    if (!input.hasQTotalCmdRaw) {
      LOG_DEBUG("AVC 控制计算跳过: group_name={}, 原因=尚未收到总无功输入", config.group_name());
      return std::nullopt;
    }

    const auto& cmdSpec = config.q_total_cmd();
    double cmdQKvar = 0.0;
    if (cmdSpec.mode() == AVCProto::VALUE_MODE_DELTA) {
      cmdQKvar = toPhysicalDelta(cmdSpec.signal(), input.qTotalCmdRaw);
    } else {
      cmdQKvar = toPhysicalAbs(cmdSpec.signal(), input.qTotalCmdRaw);
    }

    desiredTotalQKvar = cmdQKvar;
    if (cmdSpec.mode() == AVCProto::VALUE_MODE_DELTA) {
      double baseQKvar = out.totalQMeasKvar;
      switch (cmdSpec.delta_base()) {
      case AVCProto::DELTA_BASE_LAST_TARGET:
        if (input.hasLastDesiredTotalQKvar) {
          baseQKvar = input.lastDesiredTotalQKvar;
        }
        break;
      case AVCProto::DELTA_BASE_BASE_TAG: {
        auto it = input.baseRawByTag.find(cmdSpec.base_tag());
        if (it != input.baseRawByTag.end()) {
          baseQKvar = toPhysicalAbs(cmdSpec.signal(), it->second);
        }
        break;
      }
      case AVCProto::DELTA_BASE_CURRENT_MEAS:
      case AVCProto::DELTA_BASE_UNSPECIFIED:
      default:
        break;
      }
      desiredTotalQKvar = baseQKvar + cmdQKvar;
    }
  }

  const auto defaultOutput = ComputeDefaultPointOutput(config, input);
  out.rawDesiredTotalQKvar = desiredTotalQKvar;
  desiredTotalQKvar = std::clamp(desiredTotalQKvar,
                                 defaultOutput.dynamicLowerQKvar,
                                 defaultOutput.dynamicUpperQKvar);
  out.desiredTotalQKvar = desiredTotalQKvar;

  double passiveQKvar = 0.0;
  for (size_t i = 0; i < memberCount; ++i) {
    if (!config.members(static_cast<int>(i)).controllable()) {
      passiveQKvar += memberQMeasKvar[i];
    }
  }
  out.passiveQKvar = passiveQKvar;
  out.targetControllableQKvar = desiredTotalQKvar - passiveQKvar;

  std::vector<size_t> controllableIdx;
  controllableIdx.reserve(memberCount);
  std::vector<AGVC::AllocationMember> allocMembers;
  allocMembers.reserve(memberCount);
  for (size_t i = 0; i < memberCount; ++i) {
    const auto& member = config.members(static_cast<int>(i));
    if (!member.controllable()) {
      continue;
    }
    controllableIdx.emplace_back(i);
    AGVC::AllocationMember allocationMember;
    allocationMember.weight = member.weight() > 0.0 ? member.weight() : 1.0;
    allocationMember.min = member.q_min_kvar();
    allocationMember.max = member.q_max_kvar();
    allocMembers.emplace_back(allocationMember);
  }

  const auto alloc = strategy.Allocate(out.targetControllableQKvar, allocMembers);
  out.unallocatedQKvar = alloc.unallocated;

  for (size_t k = 0; k < controllableIdx.size() && k < alloc.values.size(); ++k) {
    out.memberTargetQKvar[controllableIdx[k]] = alloc.values[k];
  }

  double actualTargetQKvar = passiveQKvar;
  for (size_t i = 0; i < memberCount; ++i) {
    if (config.members(static_cast<int>(i)).controllable()) {
      actualTargetQKvar += out.memberTargetQKvar[i];
    }
  }
  out.actualTargetQKvar = actualTargetQKvar;
  out.totalQErrorKvar = out.actualTargetQKvar - out.totalQMeasKvar;

  for (size_t i = 0; i < memberCount; ++i) {
    const auto& member = config.members(static_cast<int>(i));
    if (!member.controllable()) {
      continue;
    }
    if (!member.has_q_set() || !member.q_set().has_signal() || member.q_set().signal().tag().empty()) {
      continue;
    }

    const auto& outSpec = member.q_set();
    double publishKvar = 0.0;
    if (outSpec.mode() == AVCProto::VALUE_MODE_DELTA) {
      double baseQKvar = 0.0;
      switch (outSpec.delta_base()) {
      case AVCProto::DELTA_BASE_LAST_TARGET:
        if (i < input.lastMemberTargetQKvar.size() && i < input.hasLastMemberTargetQKvar.size() &&
            input.hasLastMemberTargetQKvar[i]) {
          baseQKvar = input.lastMemberTargetQKvar[i];
        }
        break;
      case AVCProto::DELTA_BASE_CURRENT_MEAS:
        baseQKvar = memberQMeasKvar[i];
        break;
      case AVCProto::DELTA_BASE_BASE_TAG: {
        auto it = input.baseRawByTag.find(outSpec.base_tag());
        if (it != input.baseRawByTag.end()) {
          baseQKvar = toPhysicalAbs(outSpec.signal(), it->second);
        } else {
          baseQKvar = memberQMeasKvar[i];
        }
        break;
      }
      case AVCProto::DELTA_BASE_UNSPECIFIED:
      default:
        baseQKvar = memberQMeasKvar[i];
        break;
      }
      publishKvar = out.memberTargetQKvar[i] - baseQKvar;
    } else {
      publishKvar = out.memberTargetQKvar[i];
    }

    out.memberPublish[i] = true;
    out.memberPublishKvar[i] = publishKvar;
  }

  out.hasLastDesiredTotalQKvar = true;
  out.nextLastDesiredTotalQKvar = out.desiredTotalQKvar;
  out.hasLastMemberTargetQKvar.assign(memberCount, false);
  out.nextLastMemberTargetQKvar.assign(memberCount, 0.0);
  for (size_t i = 0; i < memberCount; ++i) {
    if (config.members(static_cast<int>(i)).controllable()) {
      out.hasLastMemberTargetQKvar[i] = true;
      out.nextLastMemberTargetQKvar[i] = out.memberTargetQKvar[i];
    }
  }

  LOG_DEBUG(
      "AVC 控制计算完成: group_name={}, total_q_meas_kvar={}, desired_total_q_kvar={}, actual_target_q_kvar={}, target_controllable_q_kvar={}, passive_q_kvar={}, unallocated_q_kvar={}, has_voltage_error={}",
      config.group_name(),
      out.totalQMeasKvar,
      out.desiredTotalQKvar,
      out.actualTargetQKvar,
      out.targetControllableQKvar,
      out.passiveQKvar,
      out.unallocatedQKvar,
      out.hasVoltageError);

  return out;
}

}  // namespace AVC
