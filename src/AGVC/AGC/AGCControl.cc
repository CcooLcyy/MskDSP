#include "AGCControl.h"

#include <algorithm>
#include <numeric>

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

double toRawAbs(const AGCProto::SignalSpec& s, double physical) {
  const auto scale = effectiveScale(s);
  return (physical - s.offset()) / scale;
}

double toRawDelta(const AGCProto::SignalSpec& s, double physicalDelta) {
  const auto scale = effectiveScale(s);
  return physicalDelta / scale;
}
}  // namespace

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
  out.memberPublishRaw.assign(memberCount, 0.0);

  std::vector<double> measKw(memberCount, 0.0);
  for (size_t i = 0; i < memberCount; ++i) {
    if (i < input.memberMeasRaw.size() && i < input.hasMemberMeasRaw.size() && input.hasMemberMeasRaw[i]) {
      measKw[i] = toPhysicalAbs(config.members(static_cast<int>(i)).p_meas(), input.memberMeasRaw[i]);
    }
  }

  out.totalMeasKw = std::accumulate(measKw.begin(), measKw.end(), 0.0);

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
    const auto& o = config.outputs();
    if (o.has_p_total_meas() && !o.p_total_meas().tag().empty()) {
      out.publishTotalMeas = true;
      out.totalMeasRaw = toRawAbs(o.p_total_meas(), out.totalMeasKw);
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
    a.max = m.max_kw();
    if (a.max == 0.0 && m.capacity_kw() > 0.0) {
      a.max = m.capacity_kw();
    }
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
      out.totalTargetRaw = toRawAbs(o.p_total_target(), actualTargetKw);
    }
    if (o.has_p_total_error() && !o.p_total_error().tag().empty()) {
      out.publishTotalError = true;
      out.totalErrorRaw = toRawAbs(o.p_total_error(), out.totalErrorKw);
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
    double publishRaw = 0.0;
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
      publishRaw = toRawDelta(outSpec.signal(), out.memberTargetKw[i] - baseKw);
    } else {
      publishRaw = toRawAbs(outSpec.signal(), out.memberTargetKw[i]);
    }

    out.memberPublish[i] = true;
    out.memberPublishRaw[i] = publishRaw;
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
