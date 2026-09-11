#include "AVCControl.h"

#include <expected>
#include <string_view>

#include "Logger.h"

namespace AVC {
namespace {

using Error = numeric::DecimalError;

std::expected<Decimal, Error> toPhysicalAbs(
    const AVCProto::SignalSpec& signal, const Decimal& raw) {
  auto scale = numeric::Scale(signal);
  auto offset = numeric::Offset(signal);
  if (!scale.has_value() || !offset.has_value()) {
    return std::unexpected(!scale.has_value() ? scale.error() : offset.error());
  }
  return mskdsp::numeric::ApplyEngineering(raw, *scale, *offset);
}

std::expected<Decimal, Error> toPhysicalDelta(
    const AVCProto::SignalSpec& signal, const Decimal& rawDelta) {
  auto scale = numeric::Scale(signal);
  if (!scale.has_value()) {
    return std::unexpected(scale.error());
  }
  return rawDelta.Multiply(*scale);
}

bool isVoltageMode(const AVCProto::GroupConfig& config) {
  return config.command_case() == AVCProto::GroupConfig::kVoltageCmd;
}

void logCalculationError(const AVCProto::GroupConfig& config,
                         std::string_view stage, Error error) {
  LOG_ERROR("AVC 十进制控制计算失败: group_name={}, 阶段={}, 原因={}",
            config.group_name(), stage,
            mskdsp::numeric::DecimalErrorMessage(error));
}

std::expected<Decimal, Error> computeTotalQMeasKvar(
    const AVCProto::GroupConfig& config, const ControlInput& input) {
  Decimal total;
  const auto memberCount = static_cast<size_t>(config.members_size());
  for (size_t index = 0; index < memberCount; ++index) {
    if (index >= input.memberQMeasRaw.size() ||
        index >= input.hasMemberQMeasRaw.size() ||
        !input.hasMemberQMeasRaw[index]) {
      continue;
    }
    auto measured = toPhysicalAbs(
        config.members(static_cast<int>(index)).q_meas(),
        input.memberQMeasRaw[index]);
    if (!measured.has_value()) {
      return std::unexpected(measured.error());
    }
    auto next = total.Add(*measured);
    if (!next.has_value()) {
      return std::unexpected(next.error());
    }
    total = *next;
  }
  return total;
}

}  // 命名空间结束

std::optional<Decimal> ComputeVoltageMeas(
    const AVCProto::GroupConfig& config, const ControlInput& input) {
  if (!input.hasVoltageMeasRaw) {
    return std::nullopt;
  }
  auto measured = toPhysicalAbs(config.voltage_meas(), input.voltageMeasRaw);
  if (!measured.has_value()) {
    logCalculationError(config, "换算主电压量测", measured.error());
    return std::nullopt;
  }
  return *measured;
}

std::optional<Decimal> ComputeTotalQMeasKvar(
    const AVCProto::GroupConfig& config, const ControlInput& input) {
  auto total = computeTotalQMeasKvar(config, input);
  if (!total.has_value()) {
    logCalculationError(config, "汇总成员无功量测", total.error());
    return std::nullopt;
  }
  return *total;
}

DefaultPointOutput ComputeDefaultPointOutput(
    const AVCProto::GroupConfig& config, const ControlInput& input) {
  DefaultPointOutput out;
  const auto memberCount = static_cast<size_t>(config.members_size());
  for (size_t index = 0; index < memberCount; ++index) {
    const auto& member = config.members(static_cast<int>(index));
    if (member.controllable()) {
      auto minimum = numeric::Minimum(member);
      auto maximum = numeric::Maximum(member);
      if (!minimum.has_value() || !maximum.has_value()) {
        logCalculationError(config, "读取默认上下限",
                            !minimum.has_value() ? minimum.error()
                                                 : maximum.error());
        out.dynamicQuality = DataCenterProto::QUALITY_BAD;
        continue;
      }
      auto lower = out.theoreticalLowerQKvar.Add(*minimum);
      auto upper = out.theoreticalUpperQKvar.Add(*maximum);
      if (!lower.has_value() || !upper.has_value()) {
        logCalculationError(config, "汇总默认上下限",
                            !lower.has_value() ? lower.error()
                                               : upper.error());
        out.dynamicQuality = DataCenterProto::QUALITY_BAD;
        continue;
      }
      out.theoreticalLowerQKvar = *lower;
      out.theoreticalUpperQKvar = *upper;
      continue;
    }

    ++out.uncontrollableMemberCount;
    if (index < input.memberQMeasRaw.size() &&
        index < input.hasMemberQMeasRaw.size() &&
        input.hasMemberQMeasRaw[index]) {
      auto measured =
          toPhysicalAbs(member.q_meas(), input.memberQMeasRaw[index]);
      if (measured.has_value()) {
        auto lower = out.dynamicLowerQKvar.Add(*measured);
        auto upper = out.dynamicUpperQKvar.Add(*measured);
        if (lower.has_value() && upper.has_value()) {
          out.dynamicLowerQKvar = *lower;
          out.dynamicUpperQKvar = *upper;
          continue;
        }
      }
    }
    ++out.missingUncontrollableMemberCount;
  }

  auto lower = out.dynamicLowerQKvar.Add(out.theoreticalLowerQKvar);
  auto upper = out.dynamicUpperQKvar.Add(out.theoreticalUpperQKvar);
  if (lower.has_value() && upper.has_value()) {
    out.dynamicLowerQKvar = *lower;
    out.dynamicUpperQKvar = *upper;
  } else {
    out.dynamicQuality = DataCenterProto::QUALITY_BAD;
  }
  if (out.dynamicQuality != DataCenterProto::QUALITY_BAD) {
    out.dynamicQuality = out.missingUncontrollableMemberCount == 0
                             ? DataCenterProto::QUALITY_GOOD
                             : DataCenterProto::QUALITY_BAD;
  }
  return out;
}

std::optional<ControlOutput> ComputeControlOutput(
    const AVCProto::GroupConfig& config, const ControlInput& input,
    const AGVC::WeightedStrategy& strategy) {
  const auto memberCount = static_cast<size_t>(config.members_size());
  if (memberCount == 0) {
    LOG_WARNING("AVC 控制计算跳过: group_name={}, 原因=成员列表为空",
                config.group_name());
    return std::nullopt;
  }

  ControlOutput out;
  out.memberTargetQKvar.assign(memberCount, Decimal{});
  out.memberPublish.assign(memberCount, false);
  out.memberPublishKvar.assign(memberCount, Decimal{});

  std::vector<Decimal> memberQMeasKvar(memberCount);
  for (size_t index = 0; index < memberCount; ++index) {
    if (index >= input.memberQMeasRaw.size() ||
        index >= input.hasMemberQMeasRaw.size() ||
        !input.hasMemberQMeasRaw[index]) {
      continue;
    }
    auto measured = toPhysicalAbs(
        config.members(static_cast<int>(index)).q_meas(),
        input.memberQMeasRaw[index]);
    if (!measured.has_value()) {
      logCalculationError(config, "换算成员无功量测", measured.error());
      return std::nullopt;
    }
    memberQMeasKvar[index] = *measured;
  }

  auto totalMeasured = computeTotalQMeasKvar(config, input);
  if (!totalMeasured.has_value()) {
    logCalculationError(config, "汇总成员无功量测", totalMeasured.error());
    return std::nullopt;
  }
  out.totalQMeasKvar = *totalMeasured;

  auto voltageMeasured = ComputeVoltageMeas(config, input);
  if (voltageMeasured.has_value()) {
    out.hasVoltageMeas = true;
    out.voltageMeas = *voltageMeasured;
  }

  Decimal desiredTotalQKvar;
  if (isVoltageMode(config)) {
    if (!input.hasVoltageCmdRaw) {
      LOG_DEBUG("AVC 控制计算跳过: group_name={}, 原因=尚未收到目标电压输入",
                config.group_name());
      return std::nullopt;
    }
    if (!out.hasVoltageMeas) {
      LOG_DEBUG("AVC 控制计算跳过: group_name={}, 原因=尚未收到主电压量测",
                config.group_name());
      return std::nullopt;
    }

    auto targetVoltage =
        toPhysicalAbs(config.voltage_cmd(), input.voltageCmdRaw);
    if (!targetVoltage.has_value()) {
      logCalculationError(config, "换算目标电压", targetVoltage.error());
      return std::nullopt;
    }
    auto error = targetVoltage->Subtract(out.voltageMeas);
    auto deadband = numeric::Deadband(config.voltage_control());
    auto kp = numeric::Kp(config.voltage_control());
    if (!error.has_value() || !deadband.has_value() || !kp.has_value()) {
      const auto calculationError = !error.has_value() ? error.error()
                                    : !deadband.has_value() ? deadband.error()
                                                            : kp.error();
      logCalculationError(config, "计算电压偏差", calculationError);
      return std::nullopt;
    }
    out.hasVoltageError = true;
    out.voltageError = *error;

    auto absoluteError = numeric::Absolute(*error);
    if (!absoluteError.has_value()) {
      logCalculationError(config, "计算电压偏差绝对值",
                          absoluteError.error());
      return std::nullopt;
    }
    if (*deadband > numeric::Zero() && *absoluteError <= *deadband) {
      desiredTotalQKvar = out.totalQMeasKvar;
    } else {
      auto adjustment = kp->Multiply(*error);
      if (!adjustment.has_value()) {
        logCalculationError(config, "计算目标无功调节量", adjustment.error());
        return std::nullopt;
      }
      auto desired = out.totalQMeasKvar.Add(*adjustment);
      if (!desired.has_value()) {
        logCalculationError(config, "计算目标无功", desired.error());
        return std::nullopt;
      }
      desiredTotalQKvar = *desired;
    }
  } else {
    if (!config.has_q_total_cmd() || !config.q_total_cmd().has_signal()) {
      LOG_DEBUG("AVC 控制计算跳过: group_name={}, 原因=缺少总无功命令配置",
                config.group_name());
      return std::nullopt;
    }
    if (!input.hasQTotalCmdRaw) {
      LOG_DEBUG("AVC 控制计算跳过: group_name={}, 原因=尚未收到总无功输入",
                config.group_name());
      return std::nullopt;
    }

    const auto& command = config.q_total_cmd();
    auto commandQKvar = command.mode() == AVCProto::VALUE_MODE_DELTA
                            ? toPhysicalDelta(command.signal(),
                                              input.qTotalCmdRaw)
                            : toPhysicalAbs(command.signal(),
                                            input.qTotalCmdRaw);
    if (!commandQKvar.has_value()) {
      logCalculationError(config, "换算总无功输入", commandQKvar.error());
      return std::nullopt;
    }
    desiredTotalQKvar = *commandQKvar;
    if (command.mode() == AVCProto::VALUE_MODE_DELTA) {
      auto baseQKvar = out.totalQMeasKvar;
      switch (command.delta_base()) {
        case AVCProto::DELTA_BASE_LAST_TARGET:
          if (input.hasLastDesiredTotalQKvar) {
            baseQKvar = input.lastDesiredTotalQKvar;
          }
          break;
        case AVCProto::DELTA_BASE_BASE_TAG: {
          const auto iterator = input.baseRawByTag.find(command.base_tag());
          if (iterator != input.baseRawByTag.end()) {
            auto converted =
                toPhysicalAbs(command.signal(), iterator->second);
            if (!converted.has_value()) {
              logCalculationError(config, "换算总无功增量基准",
                                  converted.error());
              return std::nullopt;
            }
            baseQKvar = *converted;
          }
          break;
        }
        case AVCProto::DELTA_BASE_CURRENT_MEAS:
        case AVCProto::DELTA_BASE_UNSPECIFIED:
        default:
          break;
      }
      auto combined = baseQKvar.Add(*commandQKvar);
      if (!combined.has_value()) {
        logCalculationError(config, "叠加总无功增量", combined.error());
        return std::nullopt;
      }
      desiredTotalQKvar = *combined;
    }
  }

  if (input.hasDesiredTotalOverride) {
    desiredTotalQKvar = input.desiredTotalOverrideQKvar;
  }

  const auto defaultOutput = ComputeDefaultPointOutput(config, input);
  out.rawDesiredTotalQKvar = desiredTotalQKvar;
  auto limitedDesired = mskdsp::numeric::Clamp(
      desiredTotalQKvar, defaultOutput.dynamicLowerQKvar,
      defaultOutput.dynamicUpperQKvar);
  if (!limitedDesired.has_value()) {
    logCalculationError(config, "执行总无功限幅", limitedDesired.error());
    return std::nullopt;
  }
  out.desiredTotalQKvar = *limitedDesired;

  for (size_t index = 0; index < memberCount; ++index) {
    if (config.members(static_cast<int>(index)).controllable()) {
      continue;
    }
    auto next = out.passiveQKvar.Add(memberQMeasKvar[index]);
    if (!next.has_value()) {
      logCalculationError(config, "汇总不可控成员无功", next.error());
      return std::nullopt;
    }
    out.passiveQKvar = *next;
  }
  auto controllableTarget = out.desiredTotalQKvar.Subtract(out.passiveQKvar);
  if (!controllableTarget.has_value()) {
    logCalculationError(config, "计算可控成员总目标",
                        controllableTarget.error());
    return std::nullopt;
  }
  out.targetControllableQKvar = *controllableTarget;

  std::vector<size_t> controllableIndexes;
  std::vector<AGVC::DecimalAllocationMember> allocationMembers;
  controllableIndexes.reserve(memberCount);
  allocationMembers.reserve(memberCount);
  for (size_t index = 0; index < memberCount; ++index) {
    const auto& member = config.members(static_cast<int>(index));
    if (!member.controllable()) {
      continue;
    }
    auto weight = numeric::Weight(member);
    auto minimum = numeric::Minimum(member);
    auto maximum = numeric::Maximum(member);
    if (!weight.has_value() || !minimum.has_value() || !maximum.has_value()) {
      const auto error = !weight.has_value() ? weight.error()
                         : !minimum.has_value() ? minimum.error()
                                                : maximum.error();
      logCalculationError(config, "读取成员分配参数", error);
      return std::nullopt;
    }
    controllableIndexes.emplace_back(index);
    allocationMembers.push_back({
        .weight = *weight > numeric::Zero() ? *weight : numeric::One(),
        .min = *minimum,
        .max = *maximum,
    });
  }

  auto allocation = strategy.AllocateDecimal(out.targetControllableQKvar,
                                              allocationMembers);
  if (!allocation.has_value()) {
    logCalculationError(config, "执行成员加权分配", allocation.error());
    return std::nullopt;
  }
  out.unallocatedQKvar = allocation->unallocated;
  for (size_t index = 0;
       index < controllableIndexes.size() && index < allocation->values.size();
       ++index) {
    out.memberTargetQKvar[controllableIndexes[index]] =
        allocation->values[index];
  }

  out.actualTargetQKvar = out.passiveQKvar;
  for (size_t index = 0; index < memberCount; ++index) {
    if (!config.members(static_cast<int>(index)).controllable()) {
      continue;
    }
    auto next = out.actualTargetQKvar.Add(out.memberTargetQKvar[index]);
    if (!next.has_value()) {
      logCalculationError(config, "汇总实际无功目标", next.error());
      return std::nullopt;
    }
    out.actualTargetQKvar = *next;
  }
  auto totalError = out.actualTargetQKvar.Subtract(out.totalQMeasKvar);
  if (!totalError.has_value()) {
    logCalculationError(config, "计算总无功偏差", totalError.error());
    return std::nullopt;
  }
  out.totalQErrorKvar = *totalError;

  for (size_t index = 0; index < memberCount; ++index) {
    const auto& member = config.members(static_cast<int>(index));
    if (!member.controllable() || !member.has_q_set() ||
        !member.q_set().has_signal() ||
        member.q_set().signal().tag().empty()) {
      continue;
    }

    auto publishQKvar =
        std::expected<Decimal, Error>(out.memberTargetQKvar[index]);
    const auto& outputSpec = member.q_set();
    if (outputSpec.mode() == AVCProto::VALUE_MODE_DELTA) {
      Decimal baseQKvar;
      switch (outputSpec.delta_base()) {
        case AVCProto::DELTA_BASE_LAST_TARGET:
          if (index < input.lastMemberTargetQKvar.size() &&
              index < input.hasLastMemberTargetQKvar.size() &&
              input.hasLastMemberTargetQKvar[index]) {
            baseQKvar = input.lastMemberTargetQKvar[index];
          }
          break;
        case AVCProto::DELTA_BASE_CURRENT_MEAS:
          baseQKvar = memberQMeasKvar[index];
          break;
        case AVCProto::DELTA_BASE_BASE_TAG: {
          const auto iterator = input.baseRawByTag.find(outputSpec.base_tag());
          if (iterator != input.baseRawByTag.end()) {
            auto converted =
                toPhysicalAbs(outputSpec.signal(), iterator->second);
            if (!converted.has_value()) {
              logCalculationError(config, "换算成员无功增量基准",
                                  converted.error());
              return std::nullopt;
            }
            baseQKvar = *converted;
          } else {
            baseQKvar = memberQMeasKvar[index];
          }
          break;
        }
        case AVCProto::DELTA_BASE_UNSPECIFIED:
        default:
          baseQKvar = memberQMeasKvar[index];
          break;
      }
      publishQKvar = out.memberTargetQKvar[index].Subtract(baseQKvar);
    }
    if (!publishQKvar.has_value()) {
      logCalculationError(config, "计算成员无功发布值", publishQKvar.error());
      return std::nullopt;
    }
    out.memberPublish[index] = true;
    out.memberPublishKvar[index] = *publishQKvar;
  }

  out.hasLastDesiredTotalQKvar = true;
  out.nextLastDesiredTotalQKvar = out.desiredTotalQKvar;
  out.hasLastMemberTargetQKvar.assign(memberCount, false);
  out.nextLastMemberTargetQKvar.assign(memberCount, Decimal{});
  for (size_t index = 0; index < memberCount; ++index) {
    if (config.members(static_cast<int>(index)).controllable()) {
      out.hasLastMemberTargetQKvar[index] = true;
      out.nextLastMemberTargetQKvar[index] = out.memberTargetQKvar[index];
    }
  }

  LOG_DEBUG(
      "AVC 控制计算完成: group_name={}, total_q_meas_kvar={}, desired_total_q_kvar={}, actual_target_q_kvar={}, target_controllable_q_kvar={}, passive_q_kvar={}, unallocated_q_kvar={}, has_voltage_error={}",
      config.group_name(), out.totalQMeasKvar.ToFixedString(),
      out.desiredTotalQKvar.ToFixedString(),
      out.actualTargetQKvar.ToFixedString(),
      out.targetControllableQKvar.ToFixedString(),
      out.passiveQKvar.ToFixedString(), out.unallocatedQKvar.ToFixedString(),
      out.hasVoltageError);

  return out;
}

}  // 命名空间 AVC
