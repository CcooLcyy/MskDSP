#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "IEC61850ProtocolStack.h"

namespace IEC61850 {

enum class GooseValueType {
  BOOLEAN,
  INTEGER,
  FLOATING,
  STRING,
  BYTES,
};

struct GooseMemberConfig {
  std::string dataRef;
  IEC61850Proto::FunctionalConstraint fc =
      IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED;
  GooseValueType valueType = GooseValueType::BOOLEAN;
};

struct GooseSubscriptionConfig {
  std::vector<std::uint16_t> appIds;
  std::string gocbRef;
  std::string dataSetRef;
  std::string goId;
  std::uint64_t configRevision = 0;
  bool allowSimulation = false;
  std::vector<GooseMemberConfig> members;
};

struct GooseMessage {
  std::uint16_t appId = 0;
  std::string gocbRef;
  std::string dataSetRef;
  std::string goId;
  std::uint64_t configRevision = 0;
  std::uint32_t timeAllowedToLiveMs = 0;
  std::uint32_t stateNumber = 0;
  std::uint32_t sequenceNumber = 0;
  bool simulation = false;
  bool needsCommissioning = false;
  IEC61850Proto::NetworkChannel channel =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  std::int64_t receiveTimestampMs = 0;
  std::vector<MmsDataValue> values;
};

enum class GooseProcessResult {
  ACCEPTED,
  // 输入已从TTL超时恢复；上层应重新发布一次当前值。
  RECOVERED,
  DUPLICATE,
  CONFLICT,
  REJECTED,
  TIMED_OUT,
  NO_CHANGE,
};

enum class GooseInputState {
  NOT_RECEIVED,
  ACTIVE,
  TIMED_OUT,
};

// 只处理已解码GOOSE消息，不执行网卡IO和外部服务调用。
class GooseStateMachine {
public:
  explicit GooseStateMachine(GooseSubscriptionConfig config);

  GooseProcessResult Process(const GooseMessage& message,
                             std::int64_t nowMs);
  GooseProcessResult CheckTimeout(std::int64_t nowMs);
  void Reset();

  GooseInputState state() const;
  std::uint32_t stateNumber() const;
  std::uint32_t sequenceNumber() const;
  const std::vector<MmsDataValue>& values() const;

private:
  bool MatchesSubscription(const GooseMessage& message) const;
  bool MatchesMembers(const GooseMessage& message) const;
  bool IsNewerSequence(const GooseMessage& message) const;

  GooseSubscriptionConfig config_;
  bool configValid_ = false;
  GooseInputState state_ = GooseInputState::NOT_RECEIVED;
  std::uint32_t stateNumber_ = 0;
  std::uint32_t sequenceNumber_ = 0;
  std::int64_t expiresAtMs_ = 0;
  std::vector<MmsDataValue> values_;
};

}  // namespace IEC61850
