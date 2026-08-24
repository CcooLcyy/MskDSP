#pragma once

#include <optional>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "AGC.pb.h"
#include "AgvcStrategy.h"
#include "DataCenter.pb.h"

namespace AGC {

struct ControlInput {
  bool hasCmdRaw{false};
  double cmdRaw{0.0};
  std::unordered_map<std::string, double> baseRawByTag;
  std::vector<bool> hasMemberMeasRaw;
  std::vector<double> memberMeasRaw;

  bool hasLastDesiredTotalKw{false};
  double lastDesiredTotalKw{0.0};

  std::vector<bool> hasLastMemberTargetKw;
  std::vector<double> lastMemberTargetKw;

  // 已确认的成员固定控制参数；缺少某个成员时沿用原始分配结果。
  AGCProto::GroupControlProfile controlProfile;
  std::vector<double> integralMemoryKw;
  double controlPeriodSeconds{1.0};
  bool integralEnabled{true};
  bool hasDesiredTotalOverride{false};
  double desiredTotalOverrideKw{0.0};
};

struct ControlOutput {
  double totalMeasKw{0.0};
  double desiredTotalKw{0.0};
  double actualTargetKw{0.0};
  double totalErrorKw{0.0};

  double passiveKw{0.0};
  double targetControllableKw{0.0};
  double unallocatedKw{0.0};

  bool publishTotalMeas{false};
  bool publishTotalTarget{false};
  bool publishTotalError{false};

  std::vector<double> memberTargetKw;
  std::vector<bool> memberPublish;
  std::vector<double> memberPublishKw;

  bool hasLastDesiredTotalKw{false};
  double nextLastDesiredTotalKw{0.0};

  std::vector<bool> hasLastMemberTargetKw;
  std::vector<double> nextLastMemberTargetKw;

  std::vector<double> nextIntegralMemoryKw;
};

struct DefaultPointOutput {
  double theoreticalLowerKw{0.0};
  double theoreticalUpperKw{0.0};
  double dynamicLowerKw{0.0};
  double dynamicUpperKw{0.0};
  DataCenterProto::Quality dynamicQuality{DataCenterProto::QUALITY_GOOD};
  size_t uncontrollableMemberCount{0};
  size_t missingUncontrollableMemberCount{0};
};

std::optional<double> ComputeTotalMeasKw(
    const AGCProto::GroupConfig& config,
    const ControlInput& input,
    double* totalMeasKwOut = nullptr);
DefaultPointOutput ComputeDefaultPointOutput(const AGCProto::GroupConfig& config, const ControlInput& input);
std::optional<ControlOutput> ComputeControlOutput(
    const AGCProto::GroupConfig& config,
    const ControlInput& input,
    const AGVC::WeightedStrategy& strategy);

}  // namespace AGC
