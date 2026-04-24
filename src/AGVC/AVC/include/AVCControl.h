#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "AVC.pb.h"
#include "AgvcStrategy.h"
#include "DataCenter.pb.h"

namespace AVC {

struct ControlInput {
  bool hasVoltageMeasRaw{false};
  double voltageMeasRaw{0.0};

  bool hasVoltageCmdRaw{false};
  double voltageCmdRaw{0.0};

  bool hasQTotalCmdRaw{false};
  double qTotalCmdRaw{0.0};

  std::unordered_map<std::string, double> baseRawByTag;
  std::vector<bool> hasMemberQMeasRaw;
  std::vector<double> memberQMeasRaw;

  bool hasLastDesiredTotalQKvar{false};
  double lastDesiredTotalQKvar{0.0};

  std::vector<bool> hasLastMemberTargetQKvar;
  std::vector<double> lastMemberTargetQKvar;
};

struct ControlOutput {
  double totalQMeasKvar{0.0};
  double desiredTotalQKvar{0.0};
  double actualTargetQKvar{0.0};
  double totalQErrorKvar{0.0};

  bool hasVoltageMeas{false};
  double voltageMeas{0.0};
  bool hasVoltageError{false};
  double voltageError{0.0};

  double passiveQKvar{0.0};
  double targetControllableQKvar{0.0};
  double unallocatedQKvar{0.0};

  std::vector<double> memberTargetQKvar;
  std::vector<bool> memberPublish;
  std::vector<double> memberPublishRaw;

  bool hasLastDesiredTotalQKvar{false};
  double nextLastDesiredTotalQKvar{0.0};

  std::vector<bool> hasLastMemberTargetQKvar;
  std::vector<double> nextLastMemberTargetQKvar;
};

struct DefaultPointOutput {
  double theoreticalLowerQKvar{0.0};
  double theoreticalUpperQKvar{0.0};
  double dynamicLowerQKvar{0.0};
  double dynamicUpperQKvar{0.0};
  DataCenterProto::Quality dynamicQuality{DataCenterProto::QUALITY_GOOD};
  size_t uncontrollableMemberCount{0};
  size_t missingUncontrollableMemberCount{0};
};

std::optional<double> ComputeVoltageMeas(const AVCProto::GroupConfig& config, const ControlInput& input);
double ComputeTotalQMeasKvar(const AVCProto::GroupConfig& config, const ControlInput& input);
DefaultPointOutput ComputeDefaultPointOutput(const AVCProto::GroupConfig& config, const ControlInput& input);
std::optional<ControlOutput> ComputeControlOutput(
    const AVCProto::GroupConfig& config,
    const ControlInput& input,
    const AGVC::WeightedStrategy& strategy);

}  // namespace AVC
