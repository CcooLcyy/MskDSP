#pragma once

#include <optional>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "AGC.pb.h"
#include "AGCNumeric.hpp"
#include "AgvcStrategy.h"
#include "DataCenter.pb.h"

namespace AGC {

using Decimal = numeric::Decimal;

struct ControlInput {
  bool hasCmdRaw{false};
  Decimal cmdRaw;
  std::unordered_map<std::string, Decimal> baseRawByTag;
  std::vector<bool> hasMemberMeasRaw;
  std::vector<Decimal> memberMeasRaw;

  bool hasLastDesiredTotalKw{false};
  Decimal lastDesiredTotalKw;

  std::vector<bool> hasLastMemberTargetKw;
  std::vector<Decimal> lastMemberTargetKw;

  // 已确认的成员固定控制参数；缺少某个成员时沿用原始分配结果。
  AGCProto::GroupControlProfile controlProfile;
  std::vector<Decimal> integralMemoryKw;
  double controlPeriodSeconds{1.0};
  // 仅 PI_EVENT 模式启用固定参数修正；周期直分配模式保持纯分配。
  bool enablePi{true};
  bool integralEnabled{true};
  bool hasDesiredTotalOverride{false};
  Decimal desiredTotalOverrideKw;
};

struct ControlOutput {
  Decimal totalMeasKw;
  Decimal desiredTotalKw;
  Decimal actualTargetKw;
  Decimal totalErrorKw;

  Decimal passiveKw;
  Decimal targetControllableKw;
  Decimal unallocatedKw;

  bool publishTotalMeas{false};
  bool publishTotalTarget{false};
  bool publishTotalError{false};

  std::vector<Decimal> memberTargetKw;
  std::vector<bool> memberPublish;
  std::vector<Decimal> memberPublishKw;

  bool hasLastDesiredTotalKw{false};
  Decimal nextLastDesiredTotalKw;

  std::vector<bool> hasLastMemberTargetKw;
  std::vector<Decimal> nextLastMemberTargetKw;

  std::vector<Decimal> nextIntegralMemoryKw;
};

struct DefaultPointOutput {
  Decimal theoreticalLowerKw;
  Decimal theoreticalUpperKw;
  Decimal dynamicLowerKw;
  Decimal dynamicUpperKw;
  DataCenterProto::Quality dynamicQuality{DataCenterProto::QUALITY_GOOD};
  size_t uncontrollableMemberCount{0};
  size_t missingUncontrollableMemberCount{0};
};

std::optional<Decimal> ComputeTotalMeasKw(
    const AGCProto::GroupConfig& config,
    const ControlInput& input,
    Decimal* totalMeasKwOut = nullptr);
DefaultPointOutput ComputeDefaultPointOutput(const AGCProto::GroupConfig& config, const ControlInput& input);
std::optional<ControlOutput> ComputeControlOutput(
    const AGCProto::GroupConfig& config,
    const ControlInput& input,
    const AGVC::WeightedStrategy& strategy);

}  // namespace AGC
