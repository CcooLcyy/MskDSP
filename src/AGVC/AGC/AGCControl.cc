#include "AGCControl.h"

#include "Logger.h"

namespace AGC {
namespace {
double effectiveScale(const AGCProto::SignalSpec& s) {
  return (s.scale() == 0.0) ? 1.0 : s.scale();
}

double toPhysicalAbs(const AGCProto::SignalSpec& s, double raw) {
  const auto scale = effectiveScale(s);
  return raw * scale + s.offset();
}

double toPhysicalDelta(const AGCProto::SignalSpec& s, double rawDelta) {
  const auto scale = effectiveScale(s);
  return rawDelta * scale;
}

double effectiveMemberMaxKw(const AGCProto::MemberConfig& member) {
  const auto maxKw = member.max_kw();
  if (maxKw != 0.0) {
    if (member.capacity_kw() > 0.0 && maxKw > member.capacity_kw()) {
      return member.capacity_kw();
    }
    return maxKw;
  }
  if (member.capacity_kw() > 0.0) {
    return member.capacity_kw();
  }
  return 0.0;
}

double computeTotalMeasKw(const AGCProto::GroupConfig& config, const ControlInput& input) {
  const auto memberCount = static_cast<size_t>(config.members_size());
  double totalMeasKw = 0.0;
  for (size_t i = 0; i < memberCount; ++i) {
    if (i < input.memberMeasRaw.size() && i < input.hasMemberMeasRaw.size() && input.hasMemberMeasRaw[i]) {
      totalMeasKw += toPhysicalAbs(config.members(static_cast<int>(i)).p_meas(), input.memberMeasRaw[i]);
    }
  }
  return totalMeasKw;
}
}  // namespace

std::optional<double> ComputeTotalMeasKw(const AGCProto::GroupConfig& config, const ControlInput& input, double* totalMeasKwOut) {
  if (!config.has_outputs()) {
    return std::nullopt;
  }
  const auto& outputs = config.outputs();
  if (!outputs.has_p_total_meas() || outputs.p_total_meas().tag().empty()) {
    return std::nullopt;
  }

  const auto totalMeasKw = computeTotalMeasKw(config, input);
  if (totalMeasKwOut != nullptr) {
    *totalMeasKwOut = totalMeasKw;
  }
  return totalMeasKw;
}

DefaultPointOutput ComputeDefaultPointOutput(const AGCProto::GroupConfig& config, const ControlInput& input) {
  DefaultPointOutput out;
  const auto memberCount = static_cast<size_t>(config.members_size());
  for (size_t i = 0; i < memberCount; ++i) {
    const auto& member = config.members(static_cast<int>(i));
    if (member.controllable()) {
      out.theoreticalLowerKw += member.min_kw();
      out.theoreticalUpperKw += effectiveMemberMaxKw(member);
      continue;
    }

    ++out.uncontrollableMemberCount;
    if (i < input.memberMeasRaw.size() && i < input.hasMemberMeasRaw.size() && input.hasMemberMeasRaw[i]) {
      const auto measKw = toPhysicalAbs(member.p_meas(), input.memberMeasRaw[i]);
      out.dynamicLowerKw += measKw;
      out.dynamicUpperKw += measKw;
      continue;
    }
    ++out.missingUncontrollableMemberCount;
  }

  out.dynamicLowerKw += out.theoreticalLowerKw;
  out.dynamicUpperKw += out.theoreticalUpperKw;
  out.dynamicQuality = (out.missingUncontrollableMemberCount == 0) ? DataCenterProto::QUALITY_GOOD
                                                                   : DataCenterProto::QUALITY_BAD;
  return out;
}

std::optional<ControlOutput> ComputeControlOutput(
    const AGCProto::GroupConfig& config,
    const ControlInput& input,
    const AGVC::WeightedStrategy& strategy) {
  if (!config.has_p_cmd() || !config.p_cmd().has_signal()) {
    LOG_DEBUG("AGC 控制计算跳过: group_name={}, 原因=缺少总设定点配置", config.group_name());
    return std::nullopt;
  }
  if (!input.hasCmdRaw) {
    LOG_DEBUG("AGC 控制计算跳过: group_name={}, 原因=尚未收到总设定输入", config.group_name());
    return std::nullopt;
  }

  const auto memberCount = static_cast<size_t>(config.members_size());
  if (memberCount == 0) {
    LOG_WARNING("AGC 控制计算跳过: group_name={}, 原因=成员列表为空", config.group_name());
    return std::nullopt;
  }

  ControlOutput out;
  out.memberTargetKw.assign(memberCount, 0.0);
  out.memberPublish.assign(memberCount, false);
  out.memberPublishKw.assign(memberCount, 0.0);

  std::vector<double> measKw(memberCount, 0.0);
  for (size_t i = 0; i < memberCount; ++i) {
    if (i < input.memberMeasRaw.size() && i < input.hasMemberMeasRaw.size() && input.hasMemberMeasRaw[i]) {
      measKw[i] = toPhysicalAbs(config.members(static_cast<int>(i)).p_meas(), input.memberMeasRaw[i]);
    }
  }

  out.totalMeasKw = computeTotalMeasKw(config, input);

  const auto& cmdSpec = config.p_cmd();
  double cmdKw = 0.0;
  if (cmdSpec.mode() == AGCProto::VALUE_MODE_DELTA) {
    cmdKw = toPhysicalDelta(cmdSpec.signal(), input.cmdRaw);
  } else {
    cmdKw = toPhysicalAbs(cmdSpec.signal(), input.cmdRaw);
  }

  double desiredTotalKw = cmdKw;
  if (cmdSpec.mode() == AGCProto::VALUE_MODE_DELTA) {
    double baseKw = out.totalMeasKw;
    switch (cmdSpec.delta_base()) {
    case AGCProto::DELTA_BASE_LAST_TARGET:
      if (input.hasLastDesiredTotalKw) {
        baseKw = input.lastDesiredTotalKw;
      }
      break;
    case AGCProto::DELTA_BASE_BASE_TAG: {
      auto it = input.baseRawByTag.find(cmdSpec.base_tag());
      if (it != input.baseRawByTag.end()) {
        baseKw = toPhysicalAbs(cmdSpec.signal(), it->second);
      }
      break;
    }
    case AGCProto::DELTA_BASE_CURRENT_MEAS:
    case AGCProto::DELTA_BASE_UNSPECIFIED:
    default:
      break;
    }
    desiredTotalKw = baseKw + cmdKw;
  }
  out.desiredTotalKw = desiredTotalKw;
  out.totalErrorKw = desiredTotalKw - out.totalMeasKw;

  if (config.has_outputs()) {
    const auto& outputs = config.outputs();
    if (outputs.has_p_total_meas() && !outputs.p_total_meas().tag().empty()) {
      out.publishTotalMeas = true;
    }
  }

  const double nextTargetKw = desiredTotalKw;

  double passiveKw = 0.0;
  for (size_t i = 0; i < memberCount; ++i) {
    if (!config.members(static_cast<int>(i)).controllable()) {
      passiveKw += measKw[i];
    }
  }
  out.passiveKw = passiveKw;
  out.targetControllableKw = nextTargetKw - passiveKw;

  std::vector<size_t> controllableIdx;
  controllableIdx.reserve(memberCount);
  std::vector<AGVC::AllocationMember> allocMembers;
  allocMembers.reserve(memberCount);
  for (size_t i = 0; i < memberCount; ++i) {
    const auto& m = config.members(static_cast<int>(i));
    if (!m.controllable()) {
      continue;
    }
    controllableIdx.emplace_back(i);

    AGVC::AllocationMember a;
    a.weight = m.weight() > 0.0 ? m.weight() : (m.capacity_kw() > 0.0 ? m.capacity_kw() : 1.0);
    a.min = m.min_kw();
    a.max = effectiveMemberMaxKw(m);
    allocMembers.emplace_back(a);
  }

  const auto alloc = strategy.Allocate(out.targetControllableKw, allocMembers);
  out.unallocatedKw = alloc.unallocated;

  for (size_t k = 0; k < controllableIdx.size() && k < alloc.values.size(); ++k) {
    out.memberTargetKw[controllableIdx[k]] = alloc.values[k];
  }

  double actualTargetKw = passiveKw;
  for (size_t i = 0; i < memberCount; ++i) {
    if (config.members(static_cast<int>(i)).controllable()) {
      actualTargetKw += out.memberTargetKw[i];
    }
  }
  out.actualTargetKw = actualTargetKw;

  LOG_DEBUG(
      "AGC 控制计算完成: group_name={}, total_meas_kw={}, desired_total_kw={}, target_controllable_kw={}, passive_kw={}, actual_target_kw={}, unallocated_kw={}",
      config.group_name(),
      out.totalMeasKw,
      out.desiredTotalKw,
      out.targetControllableKw,
      out.passiveKw,
      out.actualTargetKw,
      out.unallocatedKw);

  if (config.has_outputs()) {
    const auto& o = config.outputs();
    if (o.has_p_total_target() && !o.p_total_target().tag().empty()) {
      out.publishTotalTarget = true;
    }
    if (o.has_p_total_error() && !o.p_total_error().tag().empty()) {
      out.publishTotalError = true;
    }
  }

  for (size_t i = 0; i < memberCount; ++i) {
    const auto& m = config.members(static_cast<int>(i));
    if (!m.controllable()) {
      continue;
    }
    if (!m.has_p_set() || !m.p_set().has_signal() || m.p_set().signal().tag().empty()) {
      continue;
    }

    const auto& outSpec = m.p_set();
    double publishKw = 0.0;
    if (outSpec.mode() == AGCProto::VALUE_MODE_DELTA) {
      double baseKw = 0.0;
      switch (outSpec.delta_base()) {
      case AGCProto::DELTA_BASE_LAST_TARGET:
        if (i < input.lastMemberTargetKw.size() && i < input.hasLastMemberTargetKw.size() && input.hasLastMemberTargetKw[i]) {
          baseKw = input.lastMemberTargetKw[i];
        }
        break;
      case AGCProto::DELTA_BASE_CURRENT_MEAS:
        baseKw = measKw[i];
        break;
      case AGCProto::DELTA_BASE_BASE_TAG: {
        auto it = input.baseRawByTag.find(outSpec.base_tag());
        if (it != input.baseRawByTag.end()) {
          baseKw = toPhysicalAbs(outSpec.signal(), it->second);
        } else {
          baseKw = measKw[i];
        }
        break;
      }
      case AGCProto::DELTA_BASE_UNSPECIFIED:
      default:
        baseKw = measKw[i];
        break;
      }
      publishKw = out.memberTargetKw[i] - baseKw;
    } else {
      publishKw = out.memberTargetKw[i];
    }

    out.memberPublish[i] = true;
    out.memberPublishKw[i] = publishKw;
  }

  out.hasLastDesiredTotalKw = true;
  out.nextLastDesiredTotalKw = desiredTotalKw;
  out.hasLastMemberTargetKw.assign(memberCount, false);
  out.nextLastMemberTargetKw.assign(memberCount, 0.0);
  for (size_t i = 0; i < memberCount; ++i) {
    if (config.members(static_cast<int>(i)).controllable()) {
      out.hasLastMemberTargetKw[i] = true;
      out.nextLastMemberTargetKw[i] = out.memberTargetKw[i];
    }
  }

  return out;
}

}  // namespace AGC
