#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850.pb.h"
#include "IEC61850ProtocolStack.h"
#include "IEC61850RealtimeSignalBus.h"

namespace IEC61850 {

// 实时路径中保留的通信失效标志；由GOOSE/SV超时入口设置。
inline constexpr std::uint32_t kProtectionCommunicationInvalid = 1u << 31;

enum class ProtectionComparator : std::uint8_t {
  BOOL_TRUE = 1,
  BOOL_FALSE = 2,
  EQUAL = 3,
  NOT_EQUAL = 4,
  GREATER_THAN = 5,
  GREATER_OR_EQUAL = 6,
  LESS_THAN = 7,
  LESS_OR_EQUAL = 8,
};

struct ProtectionInputCondition {
  std::uint32_t signalId = 0;
  RealtimeSignalValueType valueType = RealtimeSignalValueType::BOOLEAN;
  ProtectionComparator comparator = ProtectionComparator::BOOL_TRUE;
  RealtimeSignalScalar expected{};
  std::uint32_t maxAgeMs = 0;
};

struct ProtectionRuleConfig {
  std::uint32_t outputSubscriptionId = 0;
  std::vector<ProtectionInputCondition> conditions;
  std::vector<std::uint32_t> interlockSignalIds;
  std::vector<ProtocolRealtimeValue> assertValues;
  std::vector<ProtocolRealtimeValue> releaseValues;
  std::uint32_t assertDelayMs = 0;
  std::uint32_t releaseDelayMs = 0;
};

struct ProtectionAction {
  std::size_t ruleIndex = 0;
  std::uint32_t outputSubscriptionId = 0;
  bool asserted = false;
  std::span<const ProtocolRealtimeValue> values;
};

struct ProtectionEngineStatistics {
  std::uint64_t acceptedInputs = 0;
  std::uint64_t unknownSignal = 0;
  std::uint64_t sessionMismatch = 0;
  std::uint64_t typeMismatch = 0;
  std::uint64_t actionsQueued = 0;
  std::uint64_t actionsDropped = 0;
  std::uint64_t actionSendFailures = 0;
  std::uint64_t rulesBlocked = 0;
};

// 将配置面规则编译为固定标量实时规则，并核对GOOSE输出模板。
grpc::Status BuildProtectionRuleConfigs(
    const IEC61850Proto::IedConfig& iedConfig,
    std::span<const ProtocolGooseSubscriptionPlan> gooseSubscriptions,
    std::span<const ProtocolGoosePublisherPlan> goosePublishers,
    std::span<const ProtocolSignalDefinition> realtimeSignals,
    std::vector<ProtectionRuleConfig>* rules);

// 仅供不含启动计划的单元测试使用；生产启动必须提供稳定引用解析所需的实时信号计划。
grpc::Status BuildProtectionRuleConfigs(
    const IEC61850Proto::IedConfig& iedConfig,
    std::span<const ProtocolGooseSubscriptionPlan> gooseSubscriptions,
    std::vector<ProtectionRuleConfig>* rules);

// 保护/联锁规则引擎。构造完成后热路径不再分配内存或访问外部服务。
class ProtectionEngine {
public:
  ProtectionEngine(std::vector<ProtectionRuleConfig> rules,
                   std::uint64_t sessionGeneration,
                   std::size_t actionQueueCapacity = 0);

  ProtectionEngine(const ProtectionEngine&) = delete;
  ProtectionEngine& operator=(const ProtectionEngine&) = delete;

  // 接受一条实时输入并重新评估规则；返回false表示输入代际或类型不接受。
  bool Process(const RealtimeSignalUpdate& update,
               std::int64_t nowNs) noexcept;
  // 处理动作/释放延时，返回本次是否有规则到期或被阻断。
  bool Tick(std::int64_t nowNs) noexcept;
  // 将固定动作队列移交给发送侧；返回实际移交数量。
  std::size_t DrainActions(std::span<ProtectionAction> actions) noexcept;
  // 由发送侧确认动作；失败时保留状态并允许后续Tick重试。
  void CompleteAction(std::size_t ruleIndex, bool asserted,
                      bool success) noexcept;
  void Reset() noexcept;

  std::size_t size() const noexcept;
  std::uint64_t sessionGeneration() const noexcept;
  const ProtectionEngineStatistics& statistics() const noexcept;

private:
  struct SignalState {
    std::uint32_t signalId = 0;
    bool seen = false;
    RealtimeSignalValueType valueType = RealtimeSignalValueType::BOOLEAN;
    std::uint32_t qualityBits = 0;
    std::int64_t timestampNs = 0;
    RealtimeSignalScalar value{};
  };

  struct RuleState {
    ProtectionRuleConfig config;
    bool asserted = false;
    bool pending = false;
    bool pendingAsserted = false;
    std::int64_t dueNs = 0;
    std::uint32_t retryCount = 0;
    bool queued = false;
    bool inFlight = false;
    bool inFlightAsserted = false;
  };

  struct QueuedAction {
    std::size_t ruleIndex = 0;
    bool asserted = false;
  };

  static bool Compare(const SignalState& state,
                     const ProtectionInputCondition& condition) noexcept;
  static std::int64_t AddDelay(std::int64_t nowNs,
                               std::uint32_t delayMs) noexcept;
  std::size_t FindSignal(std::uint32_t signalId) const noexcept;
  bool EvaluateRule(const RuleState& rule, std::int64_t nowNs) const noexcept;
  bool ApplyRule(std::size_t ruleIndex, bool desired,
                 std::int64_t nowNs) noexcept;
  bool QueueAction(std::size_t ruleIndex, bool asserted) noexcept;
  bool EvaluateAll(std::int64_t nowNs) noexcept;

  std::uint64_t sessionGeneration_ = 0;
  std::vector<SignalState> signals_;
  std::vector<RuleState> rules_;
  std::vector<QueuedAction> actionQueue_;
  std::size_t actionHead_ = 0;
  std::size_t actionTail_ = 0;
  std::size_t actionCount_ = 0;
  std::int64_t lastNowNs_ = 0;
  ProtectionEngineStatistics statistics_;
};

}  // namespace IEC61850
