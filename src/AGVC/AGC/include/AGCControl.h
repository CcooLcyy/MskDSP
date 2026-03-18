#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "AGC.pb.h"
#include "AgvcStrategy.h"

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
  double totalMeasRaw{0.0};
  bool publishTotalTarget{false};
  double totalTargetRaw{0.0};
  bool publishTotalError{false};
  double totalErrorRaw{0.0};

  std::vector<double> memberTargetKw;
  std::vector<bool> memberPublish;
  std::vector<double> memberPublishRaw;

  bool hasLastDesiredTotalKw{false};
  double nextLastDesiredTotalKw{0.0};

  std::vector<bool> hasLastMemberTargetKw;
  std::vector<double> nextLastMemberTargetKw;
};

std::optional<ControlOutput> ComputeControlOutput(
    const AGCProto::GroupConfig& config,
    const ControlInput& input,
    const AGVC::WeightedStrategy& strategy);

}  // namespace AGC
