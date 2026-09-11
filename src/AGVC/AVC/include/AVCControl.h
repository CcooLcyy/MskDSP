#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "AVC.pb.h"
#include "AVCNumeric.hpp"
#include "AgvcStrategy.h"
#include "DataCenter.pb.h"

namespace AVC {

using Decimal = numeric::Decimal;

struct ControlInput {
  bool hasVoltageMeasRaw{false};
  Decimal voltageMeasRaw;

  bool hasVoltageCmdRaw{false};
  Decimal voltageCmdRaw;

  bool hasQTotalCmdRaw{false};
  Decimal qTotalCmdRaw;

  std::unordered_map<std::string, Decimal> baseRawByTag;
  std::vector<bool> hasMemberQMeasRaw;
  std::vector<Decimal> memberQMeasRaw;

  bool hasLastDesiredTotalQKvar{false};
  Decimal lastDesiredTotalQKvar;
  bool hasDesiredTotalOverride{false};
  Decimal desiredTotalOverrideQKvar;

  std::vector<bool> hasLastMemberTargetQKvar;
  std::vector<Decimal> lastMemberTargetQKvar;
};

struct ControlOutput {
  Decimal totalQMeasKvar;
  Decimal rawDesiredTotalQKvar;
  Decimal desiredTotalQKvar;
  Decimal actualTargetQKvar;
  Decimal totalQErrorKvar;

  bool hasVoltageMeas{false};
  Decimal voltageMeas;
  bool hasVoltageError{false};
  Decimal voltageError;

  Decimal passiveQKvar;
  Decimal targetControllableQKvar;
  Decimal unallocatedQKvar;

  std::vector<Decimal> memberTargetQKvar;
  std::vector<bool> memberPublish;
  std::vector<Decimal> memberPublishKvar;

  bool hasLastDesiredTotalQKvar{false};
  Decimal nextLastDesiredTotalQKvar;

  std::vector<bool> hasLastMemberTargetQKvar;
  std::vector<Decimal> nextLastMemberTargetQKvar;
};

struct DefaultPointOutput {
  Decimal theoreticalLowerQKvar;
  Decimal theoreticalUpperQKvar;
  Decimal dynamicLowerQKvar;
  Decimal dynamicUpperQKvar;
  DataCenterProto::Quality dynamicQuality{DataCenterProto::QUALITY_GOOD};
  size_t uncontrollableMemberCount{0};
  size_t missingUncontrollableMemberCount{0};
};

std::optional<Decimal> ComputeVoltageMeas(const AVCProto::GroupConfig& config, const ControlInput& input);
std::optional<Decimal> ComputeTotalQMeasKvar(const AVCProto::GroupConfig& config, const ControlInput& input);
DefaultPointOutput ComputeDefaultPointOutput(const AVCProto::GroupConfig& config, const ControlInput& input);
std::optional<ControlOutput> ComputeControlOutput(
    const AVCProto::GroupConfig& config,
    const ControlInput& input,
    const AGVC::WeightedStrategy& strategy);

}  // namespace AVC
