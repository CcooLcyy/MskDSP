#include "AGCControl.h"

#include <cmath>
#include <expected>
#include <string_view>

#include "Logger.h"

namespace AGC {
namespace {

using Error = numeric::DecimalError;

std::expected<Decimal, Error> toPhysicalAbs(
    const AGCProto::SignalSpec &signal, const Decimal &raw) {
  auto scale = numeric::Scale(signal);
  if (!scale.has_value()) {
    return std::unexpected(scale.error());
  }
  auto offset = numeric::Offset(signal);
  if (!offset.has_value()) {
    return std::unexpected(offset.error());
  }
  return mskdsp::numeric::ApplyEngineering(raw, *scale, *offset);
}

std::expected<Decimal, Error> toPhysicalDelta(
    const AGCProto::SignalSpec &signal, const Decimal &rawDelta) {
  auto scale = numeric::Scale(signal);
  if (!scale.has_value()) {
    return std::unexpected(scale.error());
  }
  return rawDelta.Multiply(*scale);
}

std::expected<Decimal, Error> effectiveMemberMaxKw(
    const AGCProto::MemberConfig &member) {
  auto maximum = numeric::Maximum(member);
  if (!maximum.has_value()) {
    return std::unexpected(maximum.error());
  }
  auto capacity = numeric::Capacity(member);
  if (!capacity.has_value()) {
    return std::unexpected(capacity.error());
  }
  if (!maximum->IsZero()) {
    if (*capacity > numeric::Zero() && *maximum > *capacity) {
      return *capacity;
    }
    return *maximum;
  }
  return *capacity > numeric::Zero() ? *capacity : numeric::Zero();
}

const AGCProto::MemberControlProfile *findControlProfile(
    const AGCProto::GroupControlProfile &profile,
    const std::string &memberName) {
  for (const auto &member : profile.members()) {
    if (member.member_name() == memberName) {
      return &member;
    }
  }
  return nullptr;
}

std::expected<Decimal, Error> clampToMemberLimits(
    const AGCProto::MemberConfig &member, const Decimal &value) {
  auto minimum = numeric::Minimum(member);
  if (!minimum.has_value()) {
    return std::unexpected(minimum.error());
  }
  auto maximum = effectiveMemberMaxKw(member);
  if (!maximum.has_value()) {
    return std::unexpected(maximum.error());
  }
  auto result = value;
  if (result > *maximum) {
    result = *maximum;
  }
  if (result < *minimum) {
    result = *minimum;
  }
  return result;
}

std::expected<Decimal, Error> computeTotalMeasKw(
    const AGCProto::GroupConfig &config, const ControlInput &input) {
  Decimal total;
  const auto memberCount = static_cast<size_t>(config.members_size());
  for (size_t index = 0; index < memberCount; ++index) {
    if (index >= input.memberMeasRaw.size() ||
        index >= input.hasMemberMeasRaw.size() ||
        !input.hasMemberMeasRaw[index]) {
      continue;
    }
    auto measured = toPhysicalAbs(
        config.members(static_cast<int>(index)).p_meas(),
        input.memberMeasRaw[index]);
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

void logCalculationError(const AGCProto::GroupConfig &config,
                         std::string_view stage, Error error) {
  LOG_ERROR("AGC 十进制控制计算失败: group_name={}, 阶段={}, 原因={}",
            config.group_name(), stage,
            mskdsp::numeric::DecimalErrorMessage(error));
}

}  // namespace

std::optional<Decimal> ComputeTotalMeasKw(
    const AGCProto::GroupConfig &config, const ControlInput &input,
    Decimal *totalMeasKwOut) {
  if (!config.has_outputs() || !config.outputs().has_p_total_meas() ||
      config.outputs().p_total_meas().tag().empty()) {
    return std::nullopt;
  }
  auto total = computeTotalMeasKw(config, input);
  if (!total.has_value()) {
    logCalculationError(config, "汇总成员量测", total.error());
    return std::nullopt;
  }
  if (totalMeasKwOut != nullptr) {
    *totalMeasKwOut = *total;
  }
  return *total;
}

DefaultPointOutput ComputeDefaultPointOutput(
    const AGCProto::GroupConfig &config, const ControlInput &input) {
  DefaultPointOutput out;
  const auto memberCount = static_cast<size_t>(config.members_size());
  for (size_t index = 0; index < memberCount; ++index) {
    const auto &member = config.members(static_cast<int>(index));
    if (member.controllable()) {
      auto minimum = numeric::Minimum(member);
      auto maximum = effectiveMemberMaxKw(member);
      if (!minimum.has_value() || !maximum.has_value()) {
        logCalculationError(config, "计算默认上下限",
                            !minimum.has_value() ? minimum.error()
                                                 : maximum.error());
        out.dynamicQuality = DataCenterProto::QUALITY_BAD;
        continue;
      }
      auto lower = out.theoreticalLowerKw.Add(*minimum);
      auto upper = out.theoreticalUpperKw.Add(*maximum);
      if (!lower.has_value() || !upper.has_value()) {
        logCalculationError(config, "汇总默认上下限",
                            !lower.has_value() ? lower.error()
                                               : upper.error());
        out.dynamicQuality = DataCenterProto::QUALITY_BAD;
        continue;
      }
      out.theoreticalLowerKw = *lower;
      out.theoreticalUpperKw = *upper;
      continue;
    }

    ++out.uncontrollableMemberCount;
    if (index < input.memberMeasRaw.size() &&
        index < input.hasMemberMeasRaw.size() &&
        input.hasMemberMeasRaw[index]) {
      auto measured = toPhysicalAbs(member.p_meas(),
                                    input.memberMeasRaw[index]);
      if (measured.has_value()) {
        auto lower = out.dynamicLowerKw.Add(*measured);
        auto upper = out.dynamicUpperKw.Add(*measured);
        if (lower.has_value() && upper.has_value()) {
          out.dynamicLowerKw = *lower;
          out.dynamicUpperKw = *upper;
          continue;
        }
      }
    }
    ++out.missingUncontrollableMemberCount;
  }

  auto lower = out.dynamicLowerKw.Add(out.theoreticalLowerKw);
  auto upper = out.dynamicUpperKw.Add(out.theoreticalUpperKw);
  if (lower.has_value() && upper.has_value()) {
    out.dynamicLowerKw = *lower;
    out.dynamicUpperKw = *upper;
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
    const AGCProto::GroupConfig &config, const ControlInput &input,
    const AGVC::WeightedStrategy &strategy) {
  if (!config.has_p_cmd() || !config.p_cmd().has_signal()) {
    LOG_DEBUG("AGC 控制计算跳过: group_name={}, 原因=缺少总设定点配置",
              config.group_name());
    return std::nullopt;
  }
  if (!input.hasCmdRaw && !input.hasDesiredTotalOverride) {
    LOG_DEBUG("AGC 控制计算跳过: group_name={}, 原因=尚未收到总设定输入",
              config.group_name());
    return std::nullopt;
  }

  const auto memberCount = static_cast<size_t>(config.members_size());
  if (memberCount == 0) {
    LOG_WARNING("AGC 控制计算跳过: group_name={}, 原因=成员列表为空",
                config.group_name());
    return std::nullopt;
  }

  ControlOutput out;
  out.memberTargetKw.assign(memberCount, Decimal{});
  out.memberPublish.assign(memberCount, false);
  out.memberPublishKw.assign(memberCount, Decimal{});

  std::vector<Decimal> measuredKw(memberCount);
  for (size_t index = 0; index < memberCount; ++index) {
    if (index >= input.memberMeasRaw.size() ||
        index >= input.hasMemberMeasRaw.size() ||
        !input.hasMemberMeasRaw[index]) {
      continue;
    }
    auto measured = toPhysicalAbs(
        config.members(static_cast<int>(index)).p_meas(),
        input.memberMeasRaw[index]);
    if (!measured.has_value()) {
      logCalculationError(config, "换算成员量测", measured.error());
      return std::nullopt;
    }
    measuredKw[index] = *measured;
  }

  auto totalMeasured = computeTotalMeasKw(config, input);
  if (!totalMeasured.has_value()) {
    logCalculationError(config, "汇总成员量测", totalMeasured.error());
    return std::nullopt;
  }
  out.totalMeasKw = *totalMeasured;

  const auto &command = config.p_cmd();
  auto commandKw = command.mode() == AGCProto::VALUE_MODE_DELTA
                       ? toPhysicalDelta(command.signal(), input.cmdRaw)
                       : toPhysicalAbs(command.signal(), input.cmdRaw);
  if (!commandKw.has_value()) {
    logCalculationError(config, "换算总设定输入", commandKw.error());
    return std::nullopt;
  }

  auto desiredTotalKw = *commandKw;
  if (command.mode() == AGCProto::VALUE_MODE_DELTA) {
    auto baseKw = out.totalMeasKw;
    switch (command.delta_base()) {
      case AGCProto::DELTA_BASE_LAST_TARGET:
        if (input.hasLastDesiredTotalKw) {
          baseKw = input.lastDesiredTotalKw;
        }
        break;
      case AGCProto::DELTA_BASE_BASE_TAG: {
        const auto iterator = input.baseRawByTag.find(command.base_tag());
        if (iterator != input.baseRawByTag.end()) {
          auto converted = toPhysicalAbs(command.signal(), iterator->second);
          if (!converted.has_value()) {
            logCalculationError(config, "换算增量基准", converted.error());
            return std::nullopt;
          }
          baseKw = *converted;
        }
        break;
      }
      case AGCProto::DELTA_BASE_CURRENT_MEAS:
      case AGCProto::DELTA_BASE_UNSPECIFIED:
      default:
        break;
    }
    auto combined = baseKw.Add(*commandKw);
    if (!combined.has_value()) {
      logCalculationError(config, "叠加增量目标", combined.error());
      return std::nullopt;
    }
    desiredTotalKw = *combined;
  }
  if (input.hasDesiredTotalOverride) {
    desiredTotalKw = input.desiredTotalOverrideKw;
  }
  out.desiredTotalKw = desiredTotalKw;
  auto totalError = desiredTotalKw.Subtract(out.totalMeasKw);
  if (!totalError.has_value()) {
    logCalculationError(config, "计算总偏差", totalError.error());
    return std::nullopt;
  }
  out.totalErrorKw = *totalError;

  if (config.has_outputs() && config.outputs().has_p_total_meas() &&
      !config.outputs().p_total_meas().tag().empty()) {
    out.publishTotalMeas = true;
  }

  for (size_t index = 0; index < memberCount; ++index) {
    if (config.members(static_cast<int>(index)).controllable()) {
      continue;
    }
    auto next = out.passiveKw.Add(measuredKw[index]);
    if (!next.has_value()) {
      logCalculationError(config, "汇总不可控出力", next.error());
      return std::nullopt;
    }
    out.passiveKw = *next;
  }
  auto controllableTarget = desiredTotalKw.Subtract(out.passiveKw);
  if (!controllableTarget.has_value()) {
    logCalculationError(config, "计算可控目标", controllableTarget.error());
    return std::nullopt;
  }
  out.targetControllableKw = *controllableTarget;

  std::vector<size_t> controllableIndexes;
  std::vector<AGVC::DecimalAllocationMember> allocationMembers;
  controllableIndexes.reserve(memberCount);
  allocationMembers.reserve(memberCount);
  for (size_t index = 0; index < memberCount; ++index) {
    const auto &member = config.members(static_cast<int>(index));
    if (!member.controllable()) {
      continue;
    }
    auto weight = numeric::Weight(member);
    auto capacity = numeric::Capacity(member);
    auto minimum = numeric::Minimum(member);
    auto maximum = effectiveMemberMaxKw(member);
    if (!weight.has_value() || !capacity.has_value() ||
        !minimum.has_value() || !maximum.has_value()) {
      const auto error = !weight.has_value() ? weight.error()
                         : !capacity.has_value() ? capacity.error()
                         : !minimum.has_value() ? minimum.error()
                                                : maximum.error();
      logCalculationError(config, "读取成员分配参数", error);
      return std::nullopt;
    }
    controllableIndexes.emplace_back(index);
    allocationMembers.push_back({
        .weight = *weight > numeric::Zero()
                      ? *weight
                      : (*capacity > numeric::Zero() ? *capacity
                                                     : numeric::One()),
        .min = *minimum,
        .max = *maximum,
    });
  }

  auto allocation =
      strategy.AllocateDecimal(out.targetControllableKw, allocationMembers);
  if (!allocation.has_value()) {
    logCalculationError(config, "执行加权分配", allocation.error());
    return std::nullopt;
  }
  out.unallocatedKw = allocation->unallocated;
  for (size_t index = 0;
       index < controllableIndexes.size() &&
       index < allocation->values.size();
       ++index) {
    out.memberTargetKw[controllableIndexes[index]] = allocation->values[index];
  }

  out.nextIntegralMemoryKw.assign(memberCount, Decimal{});
  const auto periodSeconds =
      std::isfinite(input.controlPeriodSeconds) && input.controlPeriodSeconds > 0.0
          ? input.controlPeriodSeconds
          : 1.0;
  auto decimalPeriod = Decimal::FromDouble(periodSeconds);
  if (!decimalPeriod.has_value()) {
    logCalculationError(config, "转换控制周期", decimalPeriod.error());
    return std::nullopt;
  }
  for (size_t index = 0; index < memberCount; ++index) {
    const auto &member = config.members(static_cast<int>(index));
    if (!member.controllable()) {
      continue;
    }
    const auto *profile =
        findControlProfile(input.controlProfile, member.member_name());
    const auto previousIntegral = index < input.integralMemoryKw.size()
                                      ? input.integralMemoryKw[index]
                                      : Decimal{};
    out.nextIntegralMemoryKw[index] = previousIntegral;
    if (!input.enablePi || profile == nullptr) {
      continue;
    }

    auto memberError = out.memberTargetKw[index].Subtract(measuredKw[index]);
    if (!memberError.has_value()) {
      logCalculationError(config, "计算成员偏差", memberError.error());
      return std::nullopt;
    }
    auto errorByPeriod = memberError->Multiply(*decimalPeriod);
    if (!errorByPeriod.has_value()) {
      logCalculationError(config, "计算成员积分增量", errorByPeriod.error());
      return std::nullopt;
    }
    auto integral = input.integralEnabled
                        ? previousIntegral.Add(*errorByPeriod)
                        : std::expected<Decimal, Error>(previousIntegral);
    auto integralLimit = numeric::IntegralLimit(*profile);
    if (!integral.has_value() || !integralLimit.has_value()) {
      logCalculationError(config, "计算成员积分",
                          !integral.has_value() ? integral.error()
                                                : integralLimit.error());
      return std::nullopt;
    }
    if (*integralLimit > numeric::Zero()) {
      auto negativeLimit = numeric::Negate(*integralLimit);
      auto limited = negativeLimit.has_value()
                         ? mskdsp::numeric::Clamp(*integral, *negativeLimit,
                                                 *integralLimit)
                         : std::expected<Decimal, Error>(
                               std::unexpected(negativeLimit.error()));
      if (!limited.has_value()) {
        logCalculationError(config, "执行积分限幅", limited.error());
        return std::nullopt;
      }
      integral = *limited;
    }
    out.nextIntegralMemoryKw[index] = *integral;

    auto pGain = *memberError >= numeric::Zero()
                     ? numeric::UpPGain(*profile)
                     : numeric::DownPGain(*profile);
    auto iGain = *memberError >= numeric::Zero()
                     ? numeric::UpIGain(*profile)
                     : numeric::DownIGain(*profile);
    auto bias = *memberError >= numeric::Zero()
                    ? numeric::UpBias(*profile)
                    : numeric::DownBias(*profile);
    if (!pGain.has_value() || !iGain.has_value() || !bias.has_value()) {
      const auto error = !pGain.has_value() ? pGain.error()
                         : !iGain.has_value() ? iGain.error()
                                              : bias.error();
      logCalculationError(config, "读取成员 PI 参数", error);
      return std::nullopt;
    }
    auto pCorrection = pGain->Multiply(*memberError);
    auto iCorrection = input.integralEnabled
                           ? iGain->Multiply(*integral)
                           : std::expected<Decimal, Error>(Decimal{});
    if (!pCorrection.has_value() || !iCorrection.has_value()) {
      logCalculationError(config, "计算成员 PI 修正",
                          !pCorrection.has_value() ? pCorrection.error()
                                                   : iCorrection.error());
      return std::nullopt;
    }
    auto correction = pCorrection->Add(*iCorrection);
    if (correction.has_value()) {
      correction = *memberError >= numeric::Zero()
                       ? correction->Add(*bias)
                       : correction->Subtract(*bias);
    }
    auto maximumStep = numeric::MaximumStep(*profile);
    if (!correction.has_value() || !maximumStep.has_value()) {
      logCalculationError(config, "计算成员偏置修正",
                          !correction.has_value() ? correction.error()
                                                  : maximumStep.error());
      return std::nullopt;
    }
    if (*maximumStep > numeric::Zero()) {
      auto negativeStep = numeric::Negate(*maximumStep);
      auto limited = negativeStep.has_value()
                         ? mskdsp::numeric::Clamp(*correction, *negativeStep,
                                                 *maximumStep)
                         : std::expected<Decimal, Error>(
                               std::unexpected(negativeStep.error()));
      if (!limited.has_value()) {
        logCalculationError(config, "执行单步限幅", limited.error());
        return std::nullopt;
      }
      correction = *limited;
    }
    auto correctedTarget = out.memberTargetKw[index].Add(*correction);
    auto maximumRamp = numeric::MaximumRamp(*profile);
    if (!correctedTarget.has_value() || !maximumRamp.has_value()) {
      logCalculationError(config, "计算成员修正目标",
                          !correctedTarget.has_value() ? correctedTarget.error()
                                                       : maximumRamp.error());
      return std::nullopt;
    }
    if (*maximumRamp > numeric::Zero() &&
        index < input.hasLastMemberTargetKw.size() &&
        index < input.lastMemberTargetKw.size() &&
        input.hasLastMemberTargetKw[index]) {
      auto maximumDelta = maximumRamp->Multiply(*decimalPeriod);
      if (!maximumDelta.has_value()) {
        logCalculationError(config, "计算成员爬坡幅度", maximumDelta.error());
        return std::nullopt;
      }
      auto lower = input.lastMemberTargetKw[index].Subtract(*maximumDelta);
      auto upper = input.lastMemberTargetKw[index].Add(*maximumDelta);
      if (!lower.has_value() || !upper.has_value()) {
        logCalculationError(config, "计算成员爬坡边界",
                            !lower.has_value() ? lower.error() : upper.error());
        return std::nullopt;
      }
      auto limited =
          mskdsp::numeric::Clamp(*correctedTarget, *lower, *upper);
      if (!limited.has_value()) {
        logCalculationError(config, "执行成员爬坡限幅", limited.error());
        return std::nullopt;
      }
      correctedTarget = *limited;
    }
    auto clamped = clampToMemberLimits(member, *correctedTarget);
    if (!clamped.has_value()) {
      logCalculationError(config, "执行成员出力限幅", clamped.error());
      return std::nullopt;
    }
    out.memberTargetKw[index] = *clamped;
  }

  out.actualTargetKw = out.passiveKw;
  for (size_t index = 0; index < memberCount; ++index) {
    if (!config.members(static_cast<int>(index)).controllable()) {
      continue;
    }
    auto next = out.actualTargetKw.Add(out.memberTargetKw[index]);
    if (!next.has_value()) {
      logCalculationError(config, "汇总实际目标", next.error());
      return std::nullopt;
    }
    out.actualTargetKw = *next;
  }

  LOG_DEBUG(
      "AGC 控制计算完成: group_name={}, total_meas_kw={}, desired_total_kw={}, target_controllable_kw={}, passive_kw={}, actual_target_kw={}, unallocated_kw={}",
      config.group_name(), out.totalMeasKw.ToFixedString(),
      out.desiredTotalKw.ToFixedString(),
      out.targetControllableKw.ToFixedString(), out.passiveKw.ToFixedString(),
      out.actualTargetKw.ToFixedString(), out.unallocatedKw.ToFixedString());

  if (config.has_outputs()) {
    const auto &outputs = config.outputs();
    out.publishTotalTarget = outputs.has_p_total_target() &&
                             !outputs.p_total_target().tag().empty();
    out.publishTotalError = outputs.has_p_total_error() &&
                            !outputs.p_total_error().tag().empty();
  }

  for (size_t index = 0; index < memberCount; ++index) {
    const auto &member = config.members(static_cast<int>(index));
    if (!member.controllable() || !member.has_p_set() ||
        !member.p_set().has_signal() ||
        member.p_set().signal().tag().empty()) {
      continue;
    }
    auto publishKw = std::expected<Decimal, Error>(out.memberTargetKw[index]);
    if (member.p_set().mode() == AGCProto::VALUE_MODE_DELTA) {
      auto baseKw = Decimal{};
      switch (member.p_set().delta_base()) {
        case AGCProto::DELTA_BASE_LAST_TARGET:
          if (index < input.lastMemberTargetKw.size() &&
              index < input.hasLastMemberTargetKw.size() &&
              input.hasLastMemberTargetKw[index]) {
            baseKw = input.lastMemberTargetKw[index];
          }
          break;
        case AGCProto::DELTA_BASE_CURRENT_MEAS:
          baseKw = measuredKw[index];
          break;
        case AGCProto::DELTA_BASE_BASE_TAG: {
          const auto iterator =
              input.baseRawByTag.find(member.p_set().base_tag());
          if (iterator != input.baseRawByTag.end()) {
            auto converted =
                toPhysicalAbs(member.p_set().signal(), iterator->second);
            if (!converted.has_value()) {
              logCalculationError(config, "换算成员增量基准",
                                  converted.error());
              return std::nullopt;
            }
            baseKw = *converted;
          } else {
            baseKw = measuredKw[index];
          }
          break;
        }
        case AGCProto::DELTA_BASE_UNSPECIFIED:
        default:
          baseKw = measuredKw[index];
          break;
      }
      publishKw = out.memberTargetKw[index].Subtract(baseKw);
    }
    if (!publishKw.has_value()) {
      logCalculationError(config, "计算成员发布值", publishKw.error());
      return std::nullopt;
    }
    out.memberPublish[index] = true;
    out.memberPublishKw[index] = *publishKw;
  }

  out.hasLastDesiredTotalKw = true;
  out.nextLastDesiredTotalKw = desiredTotalKw;
  out.hasLastMemberTargetKw.assign(memberCount, false);
  out.nextLastMemberTargetKw.assign(memberCount, Decimal{});
  for (size_t index = 0; index < memberCount; ++index) {
    if (config.members(static_cast<int>(index)).controllable()) {
      out.hasLastMemberTargetKw[index] = true;
      out.nextLastMemberTargetKw[index] = out.memberTargetKw[index];
    }
  }
  return out;
}

}  // namespace AGC
