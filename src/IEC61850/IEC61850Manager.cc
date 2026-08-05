#include "IEC61850Manager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IEC61850ConfigValidation.h"
#include "IEC61850MmsSession.h"
#include "IEC61850ModelSelection.h"
#include "IEC61850RealtimePlan.h"
#include "Logger.h"

namespace IEC61850 {
namespace {

grpc::Status NotFound(std::string message) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::move(message));
}

void AdvanceSessionGeneration(uint64_t* generation) noexcept {
  if (generation == nullptr) {
    return;
  }
  ++(*generation);
  if (*generation == 0) {
    ++(*generation);
  }
}

void JoinRealtimeWorker(std::jthread* worker) noexcept {
  if (worker == nullptr || !worker->joinable()) {
    return;
  }
  if (worker->get_id() == std::this_thread::get_id()) {
    worker->detach();
    return;
  }
  try {
    worker->join();
  } catch (const std::exception& exception) {
    LOG_ERROR("IEC61850等待实时工作线程结束时发生异常: {}",
              exception.what());
  } catch (...) {
    LOG_ERROR("IEC61850等待实时工作线程结束时发生未知异常");
  }
}

IEC61850Proto::IedState ConvertSessionState(ProtocolSessionState state) {
  switch (state) {
  case ProtocolSessionState::CONNECTING:
  case ProtocolSessionState::CONNECTED:
    return IEC61850Proto::IED_STATE_STARTING;
  case ProtocolSessionState::READY:
    return IEC61850Proto::IED_STATE_RUNNING;
  case ProtocolSessionState::DEGRADED:
  case ProtocolSessionState::DISCONNECTED:
    return IEC61850Proto::IED_STATE_DEGRADED;
  case ProtocolSessionState::ERROR:
    return IEC61850Proto::IED_STATE_ERROR;
  }
  return IEC61850Proto::IED_STATE_ERROR;
}

uint64_t SaturatingIncrement(uint64_t value) noexcept {
  return value == std::numeric_limits<uint64_t>::max() ? value : value + 1;
}

constexpr std::uint32_t kRealtimeCommunicationInvalid = 1u << 31;

bool IsMmsChannelState(IEC61850Proto::ChannelState state) noexcept {
  return state == IEC61850Proto::CHANNEL_STATE_CONNECTING ||
         state == IEC61850Proto::CHANNEL_STATE_CONNECTED ||
         state == IEC61850Proto::CHANNEL_STATE_DISCONNECTED ||
         state == IEC61850Proto::CHANNEL_STATE_ERROR;
}

bool IsCancellationRequested(
    const std::shared_ptr<std::atomic_bool>& cancellation) noexcept {
  return cancellation != nullptr &&
         cancellation->load(std::memory_order_acquire);
}

grpc::Status ProtocolStackExceptionStatus(std::string_view operation) {
  return grpc::Status(
      grpc::StatusCode::INTERNAL,
      std::format("IEC61850协议栈{}接口发生异常", operation));
}

bool PointValueToBool(const DataCenterProto::PointValue& value,
                     bool* output) {
  if (output == nullptr) {
    return false;
  }
  switch (value.kind_case()) {
    case DataCenterProto::PointValue::kBoolValue:
      *output = value.bool_value();
      return true;
    case DataCenterProto::PointValue::kIntValue:
      *output = value.int_value() != 0;
      return true;
    case DataCenterProto::PointValue::kDoubleValue:
      if (!std::isfinite(value.double_value())) {
        return false;
      }
      *output = value.double_value() != 0.0;
      return true;
    default:
      return false;
  }
}

bool PointValueToInt64(const DataCenterProto::PointValue& value,
                       std::int64_t* output) {
  if (output == nullptr) {
    return false;
  }
  switch (value.kind_case()) {
    case DataCenterProto::PointValue::kIntValue:
      *output = value.int_value();
      return true;
    case DataCenterProto::PointValue::kBoolValue:
      *output = value.bool_value() ? 1 : 0;
      return true;
    case DataCenterProto::PointValue::kDoubleValue: {
      const double input = value.double_value();
      constexpr double kInt64LowerBound = -0x1p63;
      constexpr double kInt64UpperBoundExclusive = 0x1p63;
      if (!std::isfinite(input) || std::round(input) != input ||
          input < kInt64LowerBound ||
          input >= kInt64UpperBoundExclusive) {
        return false;
      }
      *output = static_cast<std::int64_t>(input);
      return true;
    }
    default:
      return false;
  }
}

bool PointValueToDouble(const DataCenterProto::PointValue& value,
                        double* output) {
  if (output == nullptr) {
    return false;
  }
  switch (value.kind_case()) {
    case DataCenterProto::PointValue::kDoubleValue:
      *output = value.double_value();
      return std::isfinite(*output);
    case DataCenterProto::PointValue::kIntValue:
      *output = static_cast<double>(value.int_value());
      return std::isfinite(*output);
    case DataCenterProto::PointValue::kBoolValue:
      *output = value.bool_value() ? 1.0 : 0.0;
      return true;
    default:
      return false;
  }
}

}  // namespace

Manager::Manager(std::filesystem::path databasePath,
                 std::shared_ptr<ProtocolStackAdapter> protocolStack) :
  store_(std::move(databasePath)),
  dataCenter_("IEC61850"),
  mmsPipeline_(&dataCenter_),
  protocolStack_(protocolStack ? std::move(protocolStack)
                               : MakeUnavailableProtocolStack()),
  callbackGate_(std::make_shared<CallbackGate>()) {
  config_.set_schema_version(1);
  callbackGate_->owner = this;
}

Manager::~Manager() {
  Shutdown();
  const auto gate = callbackGate_;
  if (gate) {
    std::lock_guard lock(gate->mutex);
    gate->owner = nullptr;
  }
}

void Manager::StopRealtimeWorkers(
    const std::shared_ptr<RealtimeRuntime>& realtime) noexcept {
  if (realtime == nullptr) {
    return;
  }
  realtime->gooseTimeoutWorker.request_stop();
  JoinRealtimeWorker(&realtime->gooseTimeoutWorker);
  // 发送器先停止接收新动作，但会处理已经取出的动作；实时消费者仍然
  // 存活，以便及时消费发送结果并释放完成队列槽位。
  if (realtime->protectionActionDispatcher != nullptr) {
    realtime->protectionActionDispatcher->Stop();
  }
  realtime->realtimeConsumerWorker.request_stop();
  JoinRealtimeWorker(&realtime->realtimeConsumerWorker);
  if (realtime->protectionActionDispatcher != nullptr &&
      realtime->protectionEngine != nullptr) {
    std::array<ProtectionActionCompletion, 32> completions;
    while (true) {
      const auto count = realtime->protectionActionDispatcher
                             ->DrainCompletions(completions);
      if (count == 0) {
        break;
      }
      for (std::size_t index = 0; index < count; ++index) {
        realtime->protectionEngine->CompleteAction(
            completions[index].ruleIndex, completions[index].asserted,
            completions[index].success);
      }
    }
  }
}

void Manager::SetDataCenterStub(
    std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  dataCenter_.SetStub(std::move(stub));
}

void Manager::SetProtocolStack(
    std::shared_ptr<ProtocolStackAdapter> protocolStack) {
  std::lock_guard operationLock(operationMutex_);
  std::lock_guard lock(mutex_);
  protocolStack_ = protocolStack ? std::move(protocolStack)
                                 : MakeUnavailableProtocolStack();
}

void Manager::HandleMmsConnectionEvent(std::string connName,
                                       uint64_t sessionGeneration,
                                       MmsConnectionEvent event) {
  IEC61850Proto::IedState appliedState = IEC61850Proto::IED_STATE_UNSPECIFIED;
  IEC61850Proto::NetworkChannel appliedChannel =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  bool ignored = false;
  bool reconnectAttempt = false;
  std::string invalidReason;
  {
    std::unique_lock lock(mutex_);
    const auto runtimeIt = runtimeByConnName_.find(connName);
    const auto* persisted = FindIed(config_, connName);
    if (runtimeIt == runtimeByConnName_.end() ||
        persisted == nullptr || !persisted->config().enable_mms() ||
        runtimeIt->second.sessionGeneration != sessionGeneration ||
        runtimeIt->second.state == IEC61850Proto::IED_STATE_STOPPING ||
        runtimeIt->second.state == IEC61850Proto::IED_STATE_STOPPED ||
        runtimeIt->second.state == IEC61850Proto::IED_STATE_PENDING_DELETE) {
      ignored = true;
    } else if (event.type == MmsConnectionEventType::RECONNECT_ATTEMPT) {
      auto& runtime = runtimeIt->second;
      const auto configured = std::find_if(
          persisted->config().channels().begin(),
          persisted->config().channels().end(), [&event](const auto& channel) {
            return channel.enabled() &&
                   channel.channel() == event.reconnectChannel;
          });
      if (event.reconnectChannel ==
              IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED ||
          configured == persisted->config().channels().end()) {
        invalidReason = "重连尝试通道未配置或未启用";
      } else {
        runtime.statistics.set_reconnect_count(
            SaturatingIncrement(runtime.statistics.reconnect_count()));
        if (event.timestampMs > 0) {
          runtime.statistics.set_last_event_ts_ms(std::max<uint64_t>(
              runtime.statistics.last_event_ts_ms(),
              static_cast<uint64_t>(event.timestampMs)));
        }
        appliedState = runtime.state;
        appliedChannel = runtime.activeChannel;
        reconnectAttempt = true;
      }
    } else {
      std::unordered_map<int, RuntimeChannelState> nextChannels;
      std::unordered_set<int> enabledChannels;
      for (const auto& channel : persisted->config().channels()) {
        const auto key = static_cast<int>(channel.channel());
        if (channel.enabled()) {
          enabledChannels.insert(key);
        } else {
          nextChannels[key].state = IEC61850Proto::CHANNEL_STATE_DISABLED;
        }
      }

      std::unordered_set<int> seenChannels;
      for (const auto& status : event.channels) {
        const auto key = static_cast<int>(status.channel);
        if (!enabledChannels.contains(key)) {
          invalidReason = "MMS状态快照包含未配置或未启用通道";
          break;
        }
        if (!seenChannels.insert(key).second) {
          invalidReason = "MMS状态快照包含重复通道";
          break;
        }
        if (!IsMmsChannelState(status.state)) {
          invalidReason = "MMS状态快照包含无效通道状态";
          break;
        }
        auto& channel = nextChannels[key];
        channel.state = status.state;
        channel.lastError = status.error;
      }
      if (invalidReason.empty() &&
          seenChannels.size() != enabledChannels.size()) {
        invalidReason = "MMS状态快照未覆盖全部已启用通道";
      }

      const auto activeKey = static_cast<int>(event.activeChannel);
      const auto activeIt = nextChannels.find(activeKey);
      const bool hasActiveMmsConnection =
          event.state == ProtocolSessionState::CONNECTED ||
          event.state == ProtocolSessionState::READY;
      if (invalidReason.empty() &&
          hasActiveMmsConnection &&
          (event.activeChannel ==
               IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED ||
           !enabledChannels.contains(activeKey) ||
           activeIt == nextChannels.end() ||
           activeIt->second.state !=
               IEC61850Proto::CHANNEL_STATE_CONNECTED)) {
        invalidReason = "MMS已连接快照的活动通道无效或未连接";
      }
      if (invalidReason.empty() &&
          !hasActiveMmsConnection &&
          event.activeChannel !=
              IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED) {
        invalidReason = "MMS非连接态快照不能声明活动通道";
      }
      if (invalidReason.empty() &&
          !hasActiveMmsConnection &&
          std::any_of(nextChannels.begin(), nextChannels.end(),
                      [](const auto& item) {
                        return item.second.state ==
                               IEC61850Proto::CHANNEL_STATE_CONNECTED;
                      })) {
        invalidReason = "MMS非连接态快照不能包含已连接通道";
      }

      if (invalidReason.empty()) {
        auto& runtime = runtimeIt->second;
        runtime.state = ConvertSessionState(event.state);
        runtime.activeChannel = event.activeChannel;
        runtime.lastError =
            hasActiveMmsConnection
                ? std::string()
                : event.error;
        runtime.channels = std::move(nextChannels);
        if (event.timestampMs > 0) {
          runtime.statistics.set_last_event_ts_ms(std::max<uint64_t>(
              runtime.statistics.last_event_ts_ms(),
              static_cast<uint64_t>(event.timestampMs)));
        }
        appliedState = runtime.state;
        appliedChannel = runtime.activeChannel;
      }
    }
  }

  if (ignored) {
    LOG_WARNING("IEC61850忽略非当前会话的连接状态事件: IED={}, 会话代际={}",
                connName, sessionGeneration);
    return;
  }
  if (!invalidReason.empty()) {
    LOG_WARNING("IEC61850拒绝无效MMS连接事件: IED={}, 原因={}", connName,
                invalidReason);
    return;
  }
  if (reconnectAttempt) {
    LOG_INFO("IEC61850已记录MMS重连尝试: IED={}, 通道={}", connName,
             static_cast<int>(event.reconnectChannel));
    return;
  }
  LOG_INFO("IEC61850已更新MMS连接快照: IED={}, 状态={}, 活动通道={}, 原因={}",
           connName, static_cast<int>(appliedState),
           static_cast<int>(appliedChannel), event.error);
}

void Manager::HandleGooseFrame(std::string connName,
                               uint64_t sessionGeneration,
                               ProtocolGooseFrameView frame) {
  std::shared_ptr<RealtimeRuntime> realtime;
  {
    std::lock_guard lock(mutex_);
    const auto runtimeIt = runtimeByConnName_.find(connName);
    if (runtimeIt == runtimeByConnName_.end() ||
        runtimeIt->second.sessionGeneration != sessionGeneration ||
        runtimeIt->second.state == IEC61850Proto::IED_STATE_STOPPING ||
        runtimeIt->second.state == IEC61850Proto::IED_STATE_STOPPED ||
        runtimeIt->second.realtimeRuntime == nullptr) {
      return;
    }
    realtime = runtimeIt->second.realtimeRuntime;
  }
  PublishGooseFrame(realtime, sessionGeneration, frame);
}

void Manager::HandleSvFrame(std::string connName, uint64_t sessionGeneration,
                            ProtocolSvFrameView frame) {
  std::shared_ptr<RealtimeRuntime> realtime;
  {
    std::lock_guard lock(mutex_);
    const auto runtimeIt = runtimeByConnName_.find(connName);
    if (runtimeIt == runtimeByConnName_.end() ||
        runtimeIt->second.sessionGeneration != sessionGeneration ||
        runtimeIt->second.state == IEC61850Proto::IED_STATE_STOPPING ||
        runtimeIt->second.state == IEC61850Proto::IED_STATE_STOPPED ||
        runtimeIt->second.realtimeRuntime == nullptr) {
      return;
    }
    realtime = runtimeIt->second.realtimeRuntime;
  }
  PublishSvFrame(realtime, sessionGeneration, frame);
}

void Manager::PublishGooseFrame(
    const std::shared_ptr<RealtimeRuntime>& realtime,
    std::uint64_t sessionGeneration, ProtocolGooseFrameView frame) noexcept {
  if (realtime == nullptr || realtime->sessionGeneration != sessionGeneration ||
      realtime->bus == nullptr || !realtime->bus->IsActive()) {
    return;
  }
  std::uint32_t communicationInvalidMask = 0xffffffffu;
  if (realtime->gooseEngine != nullptr) {
    std::size_t route = 0;
    const auto result = realtime->gooseEngine->TryProcess(
        frame, frame.receiveTimestampNs, &route);
    if (result != GooseRealtimeProcessResult::ACCEPTED &&
        result != GooseRealtimeProcessResult::RECOVERED) {
      if (result != GooseRealtimeProcessResult::DUPLICATE &&
          result != GooseRealtimeProcessResult::NO_CHANGE) {
        realtime->gooseFramesInvalid.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
    const bool recovered = result == GooseRealtimeProcessResult::RECOVERED;
    if (recovered) {
      communicationInvalidMask = ~kRealtimeCommunicationInvalid;
    }
  }
  realtime->gooseFramesReceived.fetch_add(1, std::memory_order_relaxed);
  const auto routeIt = std::find_if(
      realtime->gooseRoutes.begin(), realtime->gooseRoutes.end(),
      [&frame](const auto& route) { return route.subscriptionId == frame.subscriptionId; });
  const auto channelIndex = static_cast<std::size_t>(frame.channel);
  if (routeIt == realtime->gooseRoutes.end() ||
      frame.values.size() != routeIt->signalIds.size() ||
      frame.values.size() != routeIt->valueTypes.size() ||
      channelIndex >= routeIt->producerIndices.size() ||
      routeIt->producerIndices[channelIndex] >= realtime->producers.size()) {
    realtime->gooseFramesInvalid.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  for (std::size_t index = 0; index < frame.values.size(); ++index) {
    const auto signalId = routeIt->signalIds[index];
    if (signalId == 0 || frame.values[index].valueType != routeIt->valueTypes[index]) {
      if (signalId != 0) {
        realtime->gooseFramesInvalid.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    RealtimeSignalUpdate update;
    update.signalId = signalId;
    update.sessionGeneration = sessionGeneration;
    update.source = RealtimeSignalSource::GOOSE;
    update.valueType = static_cast<RealtimeSignalValueType>(frame.values[index].valueType);
    update.channel = frame.channel == IEC61850Proto::NETWORK_CHANNEL_A
                         ? RealtimeNetworkChannel::A
                         : frame.channel == IEC61850Proto::NETWORK_CHANNEL_B
                             ? RealtimeNetworkChannel::B
                             : RealtimeNetworkChannel::UNSPECIFIED;
    update.qualityBits = frame.values[index].qualityBits &
                         communicationInvalidMask;
    update.timestampNs = frame.values[index].timestampNs;
    update.sequence = (static_cast<std::uint64_t>(frame.stateNumber) << 32) |
                      frame.sequenceNumber;
    switch (frame.values[index].valueType) {
      case ProtocolRealtimeValueType::BOOLEAN:
        update.value.booleanValue = frame.values[index].value.booleanValue;
        break;
      case ProtocolRealtimeValueType::INTEGER:
        update.value.integerValue = frame.values[index].value.integerValue;
        break;
      case ProtocolRealtimeValueType::FLOATING:
        update.value.floatingValue = frame.values[index].value.floatingValue;
        break;
    }
    if (!realtime->producers[routeIt->producerIndices[channelIndex]]
             .TryPublish(update)) {
      realtime->gooseFramesInvalid.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Manager::PublishSvFrame(const std::shared_ptr<RealtimeRuntime>& realtime,
                             std::uint64_t sessionGeneration,
                             ProtocolSvFrameView frame) noexcept {
  if (realtime == nullptr || realtime->sessionGeneration != sessionGeneration ||
      realtime->bus == nullptr || !realtime->bus->IsActive()) {
    return;
  }
  if (realtime->svEngine == nullptr) {
    realtime->svFramesInvalid.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  std::uint32_t missingSamples = 0;
  const auto result =
      realtime->svEngine->TryProcess(frame, nullptr, &missingSamples);
  if (result != SvRealtimeProcessResult::ACCEPTED &&
      result != SvRealtimeProcessResult::SEQUENCE_GAP) {
    if (result != SvRealtimeProcessResult::DUPLICATE) {
      realtime->svFramesInvalid.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  const auto routeIt = std::find_if(
      realtime->svRoutes.begin(), realtime->svRoutes.end(),
      [&frame](const auto& route) { return route.streamId == frame.streamId; });
  const auto channelIndex = static_cast<std::size_t>(frame.channel);
  if (routeIt == realtime->svRoutes.end() ||
      frame.values.size() != routeIt->signalIds.size() ||
      frame.values.size() != routeIt->valueTypes.size() ||
      channelIndex >= routeIt->producerIndices.size() ||
      routeIt->producerIndices[channelIndex] >= realtime->producers.size()) {
    realtime->svFramesInvalid.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  realtime->svFramesReceived.fetch_add(1, std::memory_order_relaxed);
  if (result == SvRealtimeProcessResult::SEQUENCE_GAP) {
    realtime->svSamplesDropped.fetch_add(missingSamples,
                                         std::memory_order_relaxed);
  }
  for (std::size_t index = 0; index < frame.values.size(); ++index) {
    if (frame.values[index].valueType != routeIt->valueTypes[index]) {
      realtime->svFramesInvalid.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    RealtimeSignalUpdate update;
    update.signalId = routeIt->signalIds[index];
    update.sessionGeneration = sessionGeneration;
    update.source = RealtimeSignalSource::SV_DERIVED;
    update.valueType =
        static_cast<RealtimeSignalValueType>(frame.values[index].valueType);
    update.channel = frame.channel == IEC61850Proto::NETWORK_CHANNEL_A
                         ? RealtimeNetworkChannel::A
                         : frame.channel == IEC61850Proto::NETWORK_CHANNEL_B
                               ? RealtimeNetworkChannel::B
                               : RealtimeNetworkChannel::UNSPECIFIED;
    update.qualityBits = frame.values[index].qualityBits;
    update.timestampNs = frame.values[index].timestampNs;
    update.sequence = (static_cast<std::uint64_t>(frame.sampleCount) << 32) |
                      frame.asduIndex;
    switch (frame.values[index].valueType) {
      case ProtocolRealtimeValueType::BOOLEAN:
        update.value.booleanValue = frame.values[index].value.booleanValue;
        break;
      case ProtocolRealtimeValueType::INTEGER:
        update.value.integerValue = frame.values[index].value.integerValue;
        break;
      case ProtocolRealtimeValueType::FLOATING:
        update.value.floatingValue = frame.values[index].value.floatingValue;
        break;
    }
    if (!realtime->producers[routeIt->producerIndices[channelIndex]]
             .TryPublish(update)) {
      realtime->svSamplesDropped.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

grpc::Status Manager::LoadPersistedConfig() {
  IEC61850Proto::PersistedConfig loaded;
  auto status = store_.Load(&loaded);
  if (!status.ok()) {
    return status;
  }
  std::lock_guard lock(mutex_);
  config_ = std::move(loaded);
  RebuildRuntimeLocked();
  LOG_INFO("IEC61850聚合配置加载完成: 模型数={}, IED数={}, 点映射表数={}",
           config_.models_size(), config_.ieds_size(),
           config_.point_mappings_size());
  return grpc::Status::OK;
}

void Manager::RestoreConfiguredIeds() {
  std::lock_guard operationLock(operationMutex_);
  std::vector<std::string> connNames;
  {
    std::lock_guard lock(mutex_);
    for (const auto& ied : config_.ieds()) {
      if (!ied.pending_delete() &&
          (ied.desired_running() || ied.config().auto_start())) {
        connNames.emplace_back(ied.config().conn_name());
      }
    }
  }
  for (const auto& connName : connNames) {
    const auto status = StartIedOperation(connName, false);
    if (!status.ok()) {
      LOG_ERROR("IEC61850恢复IED通信功能失败: IED={}, 原因={}", connName,
                status.error_message());
    }
  }
}

void Manager::ReconcileDataCenter() {
  std::lock_guard operationLock(operationMutex_);
  const auto issues = ReconcileDataCenterOperation();
  for (const auto& issue : issues) {
    LOG_WARNING("IEC61850 DataCenter对账诊断: 编码={}, 路径={}, 原因={}",
                issue.code(), issue.path(), issue.message());
  }
}

void Manager::Shutdown() {
  struct ShutdownTarget {
    std::string connName;
    bool stopProtocol = false;
    bool stopMmsPipeline = false;
  };

  std::lock_guard operationLock(operationMutex_);
  std::vector<ShutdownTarget> targets;
  std::vector<std::shared_ptr<RealtimeRuntime>> realtimeRuntimes;
  std::shared_ptr<ProtocolStackAdapter> stack;
  {
    std::lock_guard lock(mutex_);
    shuttingDown_ = true;
    stack = protocolStack_;
    for (auto& [connName, runtime] : runtimeByConnName_) {
      if (runtime.realtimeRuntime != nullptr) {
        realtimeRuntimes.emplace_back(runtime.realtimeRuntime);
      }
      if (!runtime.protocolSessionActive && !runtime.mmsPipelineActive) {
        continue;
      }
      runtime.state = IEC61850Proto::IED_STATE_STOPPING;
      AdvanceSessionGeneration(&runtime.sessionGeneration);
      if (runtime.realtimeRuntime != nullptr &&
          runtime.realtimeRuntime->bus != nullptr) {
        runtime.realtimeRuntime->bus->Invalidate();
        runtime.realtimeRuntime->gooseTimeoutWorker.request_stop();
        runtime.realtimeRuntime->realtimeConsumerWorker.request_stop();
      }
      targets.emplace_back(connName, runtime.protocolSessionActive,
                           runtime.mmsPipelineActive);
    }
  }
  for (const auto& realtime : realtimeRuntimes) {
    StopRealtimeWorkers(realtime);
  }
  for (const auto& target : targets) {
    if (!target.stopMmsPipeline) {
      continue;
    }
    const auto status = mmsPipeline_.InvalidateIed(target.connName);
    if (!status.ok()) {
      LOG_ERROR("IEC61850关闭时使MMS报告入口失效失败: IED={}, 原因={}",
                target.connName, status.error_message());
    }
  }
  for (const auto& target : targets) {
    grpc::Status mmsStatus;
    if (target.stopMmsPipeline) {
      mmsStatus = mmsPipeline_.WaitForDeactivation(target.connName);
    }
    if (!mmsStatus.ok()) {
      LOG_ERROR("IEC61850关闭时停止MMS报告入口失败: IED={}, 原因={}",
                target.connName, mmsStatus.error_message());
    }
    grpc::Status stackStatus;
    if (target.stopProtocol) {
      try {
        stackStatus = stack->StopIed(target.connName);
      } catch (const std::exception& exception) {
        LOG_ERROR("IEC61850关闭时协议栈停止接口发生异常: IED={}, 异常信息={}",
                  target.connName, exception.what());
        stackStatus = ProtocolStackExceptionStatus("停止");
      } catch (...) {
        LOG_ERROR("IEC61850关闭时协议栈停止接口发生未知异常: IED={}",
                  target.connName);
        stackStatus = ProtocolStackExceptionStatus("停止");
      }
    }
    const bool stackStopped = !target.stopProtocol || stackStatus.ok();
    const bool mmsStopped = !target.stopMmsPipeline || mmsStatus.ok();
    if (!stackStatus.ok()) {
      LOG_ERROR("IEC61850关闭时停止协议栈会话失败: IED={}, 原因={}",
                target.connName, stackStatus.error_message());
    }
    const auto& status = stackStopped ? mmsStatus : stackStatus;
    std::lock_guard lock(mutex_);
    auto& runtime = runtimeByConnName_[target.connName];
    if (stackStopped) {
      runtime.protocolSessionActive = false;
      runtime.activeChannel = IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
      runtime.channels.clear();
    }
    if (mmsStopped) {
      runtime.mmsPipelineActive = false;
    }
    runtime.state = status.ok() ? IEC61850Proto::IED_STATE_STOPPED
                                : IEC61850Proto::IED_STATE_ERROR;
    runtime.lastError = status.ok() ? std::string() : status.error_message();
    if (!status.ok()) {
      LOG_ERROR("IEC61850关闭IED通信功能失败: IED={}, 原因={}",
                target.connName, status.error_message());
    }
  }
}

grpc::Status Manager::ApplyTargetConfig(
    const IEC61850Proto::ApplyTargetConfigRequest& request,
    IEC61850Proto::ApplyTargetConfigResponse* response) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response不能为空");
  }
  response->Clear();

  IEC61850Proto::PersistedConfig candidate;
  candidate.set_schema_version(1);
  for (const auto& target : request.models()) {
    IEC61850Proto::NormalizedSclModel parsed;
    std::vector<IEC61850Proto::ValidationIssue> issues;
    const auto status = parser_.Parse(target.model_name(), target.source_name(),
                                      target.content(), &parsed, &issues);
    for (const auto& issue : issues) {
      *response->add_issues() = issue;
    }
    if (!status.ok()) {
      return status;
    }
    *candidate.add_models() = std::move(parsed);
  }

  for (const auto& target : request.ieds()) {
    auto* persisted = candidate.add_ieds();
    *persisted->mutable_config() = target.config();
    persisted->set_desired_running(target.desired_running() ||
                                   target.config().auto_start());
    auto* mappings = candidate.add_point_mappings();
    mappings->set_conn_name(target.config().conn_name());
    for (const auto& point : target.points()) {
      *mappings->add_points() = point;
    }
  }

  std::vector<IEC61850Proto::ValidationIssue> validationIssues;
  auto status = ValidatePersistedConfig(candidate, &validationIssues);
  for (const auto& issue : validationIssues) {
    *response->add_issues() = issue;
  }
  if (!status.ok()) {
    return status;
  }

  std::lock_guard operationLock(operationMutex_);
  std::vector<std::string> activeConnections;
  std::unordered_set<std::string> restartOnRollback;
  {
    std::lock_guard lock(mutex_);
    if (shuttingDown_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850模块正在停止，不能应用完整目标态");
    }
    for (auto& persisted : *candidate.mutable_ieds()) {
      if (const auto* existing =
              FindIed(config_, persisted.config().conn_name())) {
        persisted.set_conn_id(existing->conn_id());
      }
    }
    for (const auto& [connName, runtime] : runtimeByConnName_) {
      if (runtime.state == IEC61850Proto::IED_STATE_STOPPED ||
          runtime.state == IEC61850Proto::IED_STATE_PENDING_DELETE) {
        continue;
      }
      activeConnections.emplace_back(connName);
      if (runtime.state == IEC61850Proto::IED_STATE_RUNNING ||
          runtime.state == IEC61850Proto::IED_STATE_DEGRADED ||
          runtime.state == IEC61850Proto::IED_STATE_STARTING) {
        restartOnRollback.emplace(connName);
      }
    }

    std::unordered_set<std::string> desiredConnections;
    for (const auto& persisted : candidate.ieds()) {
      const auto& connName = persisted.config().conn_name();
      desiredConnections.emplace(connName);
    }

    std::unordered_set<std::string> pendingConnectionDeletes;
    for (const auto& connName : config_.pending_connection_deletes()) {
      if (!desiredConnections.contains(connName)) {
        pendingConnectionDeletes.emplace(connName);
      }
    }
    for (const auto& existing : config_.ieds()) {
      if (!desiredConnections.contains(existing.config().conn_name())) {
        pendingConnectionDeletes.emplace(existing.config().conn_name());
      }
    }
    std::vector<std::string> orderedPendingDeletes(
        pendingConnectionDeletes.begin(), pendingConnectionDeletes.end());
    std::ranges::sort(orderedPendingDeletes);
    for (const auto& connName : orderedPendingDeletes) {
      candidate.add_pending_connection_deletes(connName);
    }
  }

  std::vector<std::string> stoppedRestorable;
  for (const auto& connName : activeConnections) {
    status = StopIedOperation(connName, false);
    if (!status.ok()) {
      for (const auto& stopped : stoppedRestorable) {
        const auto restartStatus = StartIedOperation(stopped, false);
        if (!restartStatus.ok()) {
          LOG_ERROR("IEC61850目标态回滚时恢复IED通信功能失败: IED={}, 原因={}",
                    stopped, restartStatus.error_message());
        }
      }
      return grpc::Status(
          status.error_code(),
          std::format("应用完整目标态前停止IED通信功能失败: IED={}, 原因={}",
                      connName, status.error_message()));
    }
    if (restartOnRollback.contains(connName)) {
      stoppedRestorable.emplace_back(connName);
    }
  }

  {
    std::lock_guard lock(mutex_);
    status = SaveCandidateLocked(candidate);
    if (status.ok()) {
      RebuildRuntimeLocked();
    }
  }
  if (!status.ok()) {
    for (const auto& connName : stoppedRestorable) {
      const auto restartStatus = StartIedOperation(connName, false);
      if (!restartStatus.ok()) {
        LOG_ERROR("IEC61850目标态保存失败后恢复IED通信功能失败: IED={}, 原因={}",
                  connName, restartStatus.error_message());
      }
    }
    return status;
  }

  for (const auto& issue : ReconcileDataCenterOperation()) {
    *response->add_issues() = issue;
  }

  std::vector<std::string> targetRunningConnections;
  {
    std::lock_guard lock(mutex_);
    for (const auto& ied : config_.ieds()) {
      if (!ied.pending_delete() &&
          (ied.desired_running() || ied.config().auto_start())) {
        targetRunningConnections.emplace_back(ied.config().conn_name());
      }
    }
  }
  for (const auto& connName : targetRunningConnections) {
    const auto startStatus = StartIedOperation(connName, false);
    if (!startStatus.ok()) {
      auto* issue = response->add_issues();
      issue->set_severity(IEC61850Proto::VALIDATION_SEVERITY_WARNING);
      issue->set_code("IED_START_FAILED");
      issue->set_path(std::format("/ieds/{}", connName));
      issue->set_message("IED通信功能启动失败: " +
                         startStatus.error_message());
    }
  }
  {
    std::lock_guard lock(mutex_);
    for (const auto& model : config_.models()) {
      *response->add_models() = SclParser::BuildSummary(model);
    }
    for (const auto& ied : config_.ieds()) {
      const auto fillStatus = FillIedInfoLocked(ied, response->add_ieds());
      if (!fillStatus.ok()) {
        return fillStatus;
      }
    }
  }
  LOG_INFO("IEC61850完整目标态应用完成: 模型数={}, IED数={}, 校验问题数={}",
           response->models_size(), response->ieds_size(),
           response->issues_size());
  return grpc::Status::OK;
}

grpc::Status Manager::ImportScl(
    const IEC61850Proto::ImportSclRequest& request,
    IEC61850Proto::ImportSclResponse* response) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response不能为空");
  }
  IEC61850Proto::NormalizedSclModel parsed;
  std::vector<IEC61850Proto::ValidationIssue> issues;
  auto status = parser_.Parse(request.model_name(), request.source_name(),
                              request.content(), &parsed, &issues);
  response->Clear();
  for (const auto& issue : issues) {
    *response->add_issues() = issue;
  }
  *response->mutable_summary() = SclParser::BuildSummary(parsed);
  if (!status.ok() || request.validate_only()) {
    return status;
  }

  std::lock_guard operationLock(operationMutex_);
  std::lock_guard lock(mutex_);
  IEC61850Proto::PersistedConfig candidate = config_;
  IEC61850Proto::NormalizedSclModel* existing = nullptr;
  for (auto& model : *candidate.mutable_models()) {
    if (model.model_name() == parsed.model_name()) {
      existing = &model;
      break;
    }
  }
  if (existing != nullptr) {
    if (existing->source_checksum() == parsed.source_checksum()) {
      *response->mutable_summary() = SclParser::BuildSummary(*existing);
      return grpc::Status::OK;
    }
    if (!request.replace()) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                          "同名模型已存在且内容不同");
    }
    for (const auto& ied : candidate.ieds()) {
      const auto runtimeIt = runtimeByConnName_.find(ied.config().conn_name());
      if (ied.config().model_name() == parsed.model_name() &&
          runtimeIt != runtimeByConnName_.end() &&
          runtimeIt->second.state != IEC61850Proto::IED_STATE_STOPPED &&
          runtimeIt->second.state != IEC61850Proto::IED_STATE_ERROR) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "运行中的IED正在使用该模型，不能替换");
      }
    }
    *existing = parsed;
  } else {
    *candidate.add_models() = parsed;
  }

  status = SaveCandidateLocked(candidate);
  if (status.ok()) {
    LOG_INFO("IEC61850模型导入成功: 模型={}, 来源={}, IED数={}",
             parsed.model_name(), parsed.source_name(), parsed.ieds_size());
  }
  return status;
}

grpc::Status Manager::GetModelSummary(
    const std::string& modelName,
    IEC61850Proto::SclModelSummary* response) const {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response不能为空");
  }
  auto status = ValidateName(modelName, "model_name");
  if (!status.ok()) {
    return status;
  }
  std::lock_guard lock(mutex_);
  const auto* model = FindModel(config_, modelName);
  if (model == nullptr) {
    return NotFound("模型不存在");
  }
  *response = SclParser::BuildSummary(*model);
  return grpc::Status::OK;
}

grpc::Status Manager::ListModels(
    IEC61850Proto::ListModelsResponse* response) const {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response不能为空");
  }
  std::lock_guard lock(mutex_);
  response->Clear();
  std::vector<const IEC61850Proto::NormalizedSclModel*> models;
  models.reserve(config_.models_size());
  for (const auto& model : config_.models()) {
    models.emplace_back(&model);
  }
  std::ranges::sort(models, {},
                    &IEC61850Proto::NormalizedSclModel::model_name);
  for (const auto* model : models) {
    *response->add_models() = SclParser::BuildSummary(*model);
  }
  return grpc::Status::OK;
}

grpc::Status Manager::DeleteModel(const std::string& modelName) {
  auto status = ValidateName(modelName, "model_name");
  if (!status.ok()) {
    return status;
  }
  std::lock_guard operationLock(operationMutex_);
  std::lock_guard lock(mutex_);
  IEC61850Proto::PersistedConfig candidate = config_;
  for (const auto& ied : candidate.ieds()) {
    if (ied.config().model_name() == modelName) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "模型仍被IED配置引用");
    }
  }
  auto* models = candidate.mutable_models();
  const auto oldSize = models->size();
  for (int index = models->size() - 1; index >= 0; --index) {
    if (models->Get(index).model_name() == modelName) {
      models->DeleteSubrange(index, 1);
    }
  }
  if (models->size() == oldSize) {
    return grpc::Status::OK;
  }
  return SaveCandidateLocked(candidate);
}

grpc::Status Manager::UpsertIed(
    const IEC61850Proto::UpsertIedRequest& request,
    IEC61850Proto::IedInfo* response) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response不能为空");
  }
  const auto& requested = request.config();
  auto status = ValidateName(requested.conn_name(), "conn_name");
  if (!status.ok()) {
    return status;
  }

  std::lock_guard operationLock(operationMutex_);
  {
    std::lock_guard lock(mutex_);
    IEC61850Proto::PersistedConfig candidate = config_;
    auto* persisted = FindIed(&candidate, requested.conn_name());
    if (persisted != nullptr && request.create_only()) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "IED连接已存在");
    }
    if (persisted != nullptr && persisted->pending_delete()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED配置处于待删除状态，不能更新");
    }
    const auto runtimeIt = runtimeByConnName_.find(requested.conn_name());
    if (runtimeIt != runtimeByConnName_.end() &&
        (runtimeIt->second.protocolSessionActive ||
         runtimeIt->second.mmsPipelineActive ||
         (runtimeIt->second.state != IEC61850Proto::IED_STATE_STOPPED &&
          runtimeIt->second.state != IEC61850Proto::IED_STATE_ERROR))) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED通信资源尚未停止，不能修改配置");
    }

    const uint32_t existingConnId =
        persisted == nullptr ? 0 : persisted->conn_id();
    const bool desiredRunning =
        persisted != nullptr && persisted->desired_running();
    if (persisted == nullptr) {
      persisted = candidate.add_ieds();
    }
    *persisted->mutable_config() = requested;
    persisted->set_conn_id(existingConnId);
    persisted->set_desired_running(desiredRunning);
    persisted->set_pending_delete(false);

    std::vector<IEC61850Proto::ValidationIssue> issues;
    status = ValidatePersistedConfig(candidate, &issues);
    if (!status.ok()) {
      return status;
    }
    status = SaveCandidateLocked(candidate);
    if (!status.ok()) {
      return status;
    }
  }

  for (const auto& issue : ReconcileDataCenterOperation()) {
    LOG_WARNING("IEC61850保存IED配置后的DataCenter对账失败: IED={}, 编码={}, 原因={}",
                requested.conn_name(), issue.code(), issue.message());
  }
  std::lock_guard lock(mutex_);
  return FillIedInfoLocked(*FindIed(&config_, requested.conn_name()), response);
}

grpc::Status Manager::GetIed(const std::string& connName,
                             IEC61850Proto::IedInfo* response) const {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response不能为空");
  }
  auto status = ValidateName(connName, "conn_name");
  if (!status.ok()) {
    return status;
  }
  std::lock_guard lock(mutex_);
  const auto* ied = FindIed(config_, connName);
  if (ied == nullptr) {
    return NotFound("IED连接不存在");
  }
  return FillIedInfoLocked(*ied, response);
}

grpc::Status Manager::ListIeds(
    IEC61850Proto::ListIedsResponse* response) const {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response不能为空");
  }
  std::lock_guard lock(mutex_);
  response->Clear();
  std::vector<const IEC61850Proto::PersistedIed*> ieds;
  ieds.reserve(config_.ieds_size());
  for (const auto& ied : config_.ieds()) {
    ieds.emplace_back(&ied);
  }
  std::ranges::sort(ieds, {}, [](const auto* ied) {
    return ied->config().conn_name();
  });
  for (const auto* ied : ieds) {
    auto status = FillIedInfoLocked(*ied, response->add_ieds());
    if (!status.ok()) {
      return status;
    }
  }
  return grpc::Status::OK;
}

grpc::Status Manager::DeleteIed(const std::string& connName) {
  auto status = ValidateName(connName, "conn_name");
  if (!status.ok()) {
    return status;
  }
  std::lock_guard operationLock(operationMutex_);
  bool needsStop = false;
  bool alreadyPendingDelete = false;
  {
    std::lock_guard lock(mutex_);
    const auto* existing = FindIed(config_, connName);
    if (existing == nullptr) {
      return grpc::Status::OK;
    }
    alreadyPendingDelete = existing->pending_delete();
    const auto runtimeIt = runtimeByConnName_.find(connName);
    needsStop = !alreadyPendingDelete &&
                runtimeIt != runtimeByConnName_.end() &&
                runtimeIt->second.state != IEC61850Proto::IED_STATE_STOPPED;
  }

  if (alreadyPendingDelete) {
    return FinalizePendingDeleteOperation(connName);
  }

  if (needsStop) {
    status = StopIedOperation(connName, true);
    if (!status.ok()) {
      LOG_ERROR("IEC61850删除IED前停止通信功能失败: IED={}, 原因={}",
                connName, status.error_message());
      return status;
    }
  }

  {
    std::lock_guard lock(mutex_);
    IEC61850Proto::PersistedConfig candidate = config_;
    auto* existing = FindIed(&candidate, connName);
    if (existing == nullptr) {
      return grpc::Status::OK;
    }
    existing->set_pending_delete(true);
    existing->set_desired_running(false);
    status = SaveCandidateLocked(candidate);
    if (!status.ok()) {
      return status;
    }
    runtimeByConnName_[connName].state =
        IEC61850Proto::IED_STATE_PENDING_DELETE;
  }

  return FinalizePendingDeleteOperation(connName);
}

grpc::Status Manager::FinalizePendingDeleteOperation(
    const std::string& connName) {
  {
    std::lock_guard lock(mutex_);
    const auto* ied = FindIed(config_, connName);
    if (ied != nullptr && !ied->pending_delete()) {
      IEC61850Proto::PersistedConfig candidate = config_;
      auto* pendingDeletes = candidate.mutable_pending_connection_deletes();
      for (int index = pendingDeletes->size() - 1; index >= 0; --index) {
        if (pendingDeletes->Get(index) == connName) {
          pendingDeletes->DeleteSubrange(index, 1);
        }
      }
      const auto status = SaveCandidateLocked(candidate);
      if (status.ok()) {
        LOG_WARNING("IEC61850取消与当前IED冲突的过期待删除连接: IED={}",
                    connName);
      }
      return status;
    }
  }

  const auto dataCenterStatus = dataCenter_.DeleteConnection(connName);
  if (!dataCenterStatus.ok() &&
      dataCenterStatus.error_code() != grpc::StatusCode::NOT_FOUND) {
    std::lock_guard lock(mutex_);
    const auto runtimeIt = runtimeByConnName_.find(connName);
    if (runtimeIt != runtimeByConnName_.end()) {
      runtimeIt->second.dataCenterAvailable = false;
      runtimeIt->second.dataCenterError =
          "DataCenter连接删除失败: " + dataCenterStatus.error_message();
    }
    return dataCenterStatus;
  }

  bool removedIed = false;
  {
    std::lock_guard lock(mutex_);
    IEC61850Proto::PersistedConfig candidate = config_;
    auto* ieds = candidate.mutable_ieds();
    for (int index = ieds->size() - 1; index >= 0; --index) {
      if (ieds->Get(index).config().conn_name() == connName &&
          ieds->Get(index).pending_delete()) {
        ieds->DeleteSubrange(index, 1);
      }
    }
    auto* mappings = candidate.mutable_point_mappings();
    if (FindIed(candidate, connName) == nullptr) {
      for (int index = mappings->size() - 1; index >= 0; --index) {
        if (mappings->Get(index).conn_name() == connName) {
          mappings->DeleteSubrange(index, 1);
        }
      }
    }
    auto* pendingDeletes = candidate.mutable_pending_connection_deletes();
    for (int index = pendingDeletes->size() - 1; index >= 0; --index) {
      if (pendingDeletes->Get(index) == connName) {
        pendingDeletes->DeleteSubrange(index, 1);
      }
    }
    const auto saveStatus = SaveCandidateLocked(candidate);
    if (!saveStatus.ok()) {
      return saveStatus;
    }
    removedIed = FindIed(config_, connName) == nullptr;
    if (removedIed) {
      runtimeByConnName_.erase(connName);
    }
  }
  if (removedIed) {
    mmsPipeline_.RemoveIed(connName);
  }
  LOG_INFO("IEC61850已完成待删除连接清理: IED={}", connName);
  return grpc::Status::OK;
}

grpc::Status Manager::StartIed(const std::string& connName) {
  auto status = ValidateName(connName, "conn_name");
  if (!status.ok()) {
    return status;
  }
  std::lock_guard operationLock(operationMutex_);
  return StartIedOperation(connName, true);
}

grpc::Status Manager::StartIedOperation(const std::string& connName,
                                        bool persistDesiredState) {
  grpc::Status status;
  IEC61850Proto::IedConfig iedConfig;
  ProtocolIedPlan plan;
  IEC61850Proto::PointMappings mappings;
  uint32_t connId = 0;
  uint64_t sessionGeneration = 0;
  std::shared_ptr<RealtimeRuntime> realtimeRuntime;
  std::shared_ptr<ProtocolStackAdapter> stack;
  {
    std::unique_lock lock(mutex_);
    if (shuttingDown_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850模块正在停止，不能启动IED通信功能");
    }
    auto* persisted = FindIed(&config_, connName);
    if (persisted == nullptr) {
      return NotFound("IED连接不存在");
    }
    if (persisted->pending_delete()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED配置处于待删除状态，不能启动通信功能");
    }
    auto& runtime = runtimeByConnName_[connName];
    if (runtime.protocolSessionActive &&
        (runtime.state == IEC61850Proto::IED_STATE_STARTING ||
         runtime.state == IEC61850Proto::IED_STATE_RUNNING ||
         runtime.state == IEC61850Proto::IED_STATE_DEGRADED)) {
      return grpc::Status::OK;
    }
    if (runtime.protocolSessionActive) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          "IED协议栈会话仍处于活动状态，请先停止通信功能后再启动");
    }
    if (runtime.mmsPipelineActive) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          "IED旧MMS发布管线仍在收敛，请先停止通信功能后再启动");
    }
    const auto* foundModel = FindModel(config_, persisted->config().model_name());
    if (foundModel == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED引用模型不存在");
    }
    iedConfig = persisted->config();
    plan.config = iedConfig;
    status = BuildThreadRuntimePolicy(iedConfig, &plan.realtimePolicy);
    if (!status.ok()) {
      LOG_ERROR("IEC61850 IED实时线程策略校验失败: IED={}, 原因={}", connName,
                status.error_message());
      return status;
    }
    const auto selectedIed = std::find_if(
        foundModel->ieds().begin(), foundModel->ieds().end(),
        [&iedConfig](const auto& candidate) {
          return candidate.name() == iedConfig.ied_name();
        });
    if (selectedIed == foundModel->ieds().end()) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("模型中不存在当前IED: {}", iedConfig.ied_name()));
    }
    status = BuildAccessPointIedModel(*selectedIed,
                                      iedConfig.access_point(), &plan.ied);
    if (!status.ok()) {
      return status;
    }
    std::vector<const IEC61850Proto::SclConnectedAp*> matchingConnectedAps;
    for (const auto& connectedAp : foundModel->connected_access_points()) {
      if (connectedAp.ied_name() == iedConfig.ied_name() &&
          connectedAp.ap_name() == iedConfig.access_point()) {
        matchingConnectedAps.emplace_back(&connectedAp);
      }
    }
    if (matchingConnectedAps.empty()) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format(
              "模型通信段中不存在当前IED和AccessPoint对应的ConnectedAP: IED={}, AccessPoint={}",
              iedConfig.ied_name(), iedConfig.access_point()));
    }
    std::unordered_set<std::string> selectedSubnetworks;
    for (const auto& channel : iedConfig.channels()) {
      if (!channel.enabled()) {
        continue;
      }
      const IEC61850Proto::SclConnectedAp* selected = nullptr;
      if (channel.subnetwork_name().empty()) {
        if (matchingConnectedAps.size() != 1) {
          return grpc::Status(
              grpc::StatusCode::FAILED_PRECONDITION,
              std::format(
                  "IED通道存在多个可选ConnectedAP，必须配置subnetwork_name: IED={}, 通道={}",
                  connName, static_cast<int>(channel.channel())));
        }
        selected = matchingConnectedAps.front();
      } else {
        for (const auto* candidate : matchingConnectedAps) {
          if (candidate->subnetwork_name() != channel.subnetwork_name()) {
            continue;
          }
          if (selected != nullptr) {
            return grpc::Status(
                grpc::StatusCode::FAILED_PRECONDITION,
                std::format(
                    "IED通道的subnetwork_name匹配到重复ConnectedAP: IED={}, 通道={}, 网段={}",
                    connName, static_cast<int>(channel.channel()),
                    channel.subnetwork_name()));
          }
          selected = candidate;
        }
        if (selected == nullptr) {
          return grpc::Status(
              grpc::StatusCode::FAILED_PRECONDITION,
              std::format(
                  "IED通道的subnetwork_name在模型中不存在: IED={}, 通道={}, 网段={}",
                  connName, static_cast<int>(channel.channel()),
                  channel.subnetwork_name()));
        }
      }
      auto& binding = plan.networkBindings.emplace_back();
      binding.channel = channel;
      binding.connectedAccessPoint = *selected;
      if (selectedSubnetworks.emplace(selected->subnetwork_name()).second) {
        plan.connectedAccessPoints.emplace_back(*selected);
      }
    }
    if (plan.networkBindings.empty()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED没有可用于启动通信功能的网络通道绑定");
    }
    connId = persisted->conn_id();
    mappings.set_conn_name(connName);
    if (const auto* foundMappings = FindMappings(config_, connName)) {
      mappings = *foundMappings;
    }
    status = BuildRealtimeProtocolPlan(*foundModel, mappings, &plan);
    if (!status.ok()) {
      return status;
    }
    std::vector<ProtectionRuleConfig> protectionRules;
    status = BuildProtectionRuleConfigs(
        iedConfig,
        std::span<const ProtocolGooseSubscriptionPlan>(
            plan.gooseSubscriptions.data(), plan.gooseSubscriptions.size()),
        std::span<const ProtocolGoosePublisherPlan>(
            plan.goosePublishers.data(), plan.goosePublishers.size()),
        std::span<const ProtocolSignalDefinition>(plan.realtimeSignals.data(),
                                                  plan.realtimeSignals.size()),
        &protectionRules);
    if (!status.ok()) {
      LOG_ERROR("IEC61850保护规则编译失败: IED={}, 原因={}", connName,
                status.error_message());
      return status;
    }
    std::size_t maxProtectionActionValues = 0;
    for (const auto& rule : protectionRules) {
      maxProtectionActionValues = std::max(
          maxProtectionActionValues,
          std::max(rule.assertValues.size(), rule.releaseValues.size()));
    }
    if (persistDesiredState) {
      IEC61850Proto::PersistedConfig candidate = config_;
      FindIed(&candidate, connName)->set_desired_running(true);
      status = SaveCandidateLocked(candidate);
      if (!status.ok()) {
        return status;
      }
    }
    runtime.state = IEC61850Proto::IED_STATE_STARTING;
    runtime.lastError.clear();
    runtime.activeChannel = IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
    AdvanceSessionGeneration(&runtime.sessionGeneration);
    sessionGeneration = runtime.sessionGeneration;
    runtime.protocolSessionActive = false;
    runtime.mmsPipelineActive = false;
    const auto previousRealtimeRuntime = runtime.realtimeRuntime;
    StopRealtimeWorkers(previousRealtimeRuntime);
    runtime.realtimeRuntime = std::make_shared<RealtimeRuntime>();
    if (previousRealtimeRuntime != nullptr) {
      runtime.realtimeRuntime->gooseFramesReceived.store(
          previousRealtimeRuntime->gooseFramesReceived.load(
              std::memory_order_acquire),
          std::memory_order_release);
      runtime.realtimeRuntime->gooseFramesInvalid.store(
          previousRealtimeRuntime->gooseFramesInvalid.load(
              std::memory_order_acquire),
          std::memory_order_release);
      runtime.realtimeRuntime->gooseTimeouts.store(
          previousRealtimeRuntime->gooseTimeouts.load(
              std::memory_order_acquire),
          std::memory_order_release);
      runtime.realtimeRuntime->svFramesReceived.store(
          previousRealtimeRuntime->svFramesReceived.load(
              std::memory_order_acquire),
          std::memory_order_release);
      runtime.realtimeRuntime->svFramesInvalid.store(
          previousRealtimeRuntime->svFramesInvalid.load(
              std::memory_order_acquire),
          std::memory_order_release);
      runtime.realtimeRuntime->svSamplesDropped.store(
          previousRealtimeRuntime->svSamplesDropped.load(
              std::memory_order_acquire),
          std::memory_order_release);
    }
    runtime.realtimeRuntime->sessionGeneration = sessionGeneration;
    runtime.realtimeRuntime->connName = connName;
    runtime.realtimeRuntime->runtimePolicy = plan.realtimePolicy;
    runtime.realtimeRuntime->protocolStack = protocolStack_;
    runtime.realtimeRuntime->signalProcessor =
        std::make_shared<RealtimeSignalProcessor>(plan.realtimeSignals,
                                                   sessionGeneration);
    if (!protectionRules.empty()) {
      runtime.realtimeRuntime->protectionEngine =
          std::make_shared<ProtectionEngine>(std::move(protectionRules),
                                             sessionGeneration);
      const auto protectionEngine =
          runtime.realtimeRuntime->protectionEngine;
      const std::weak_ptr<RealtimeRuntime> weakRealtime =
          runtime.realtimeRuntime;
      const auto actionQueueCapacity = std::max<std::size_t>(
          16, protectionEngine->size() * 4);
      runtime.realtimeRuntime->protectionActionDispatcher =
          std::make_shared<ProtectionActionDispatcher>(
              protectionEngine, connName, actionQueueCapacity,
              maxProtectionActionValues,
              [weakRealtime](std::uint32_t publisherId,
                             std::span<const ProtocolRealtimeValue> values,
                             bool stateChanged) {
                const auto realtime = weakRealtime.lock();
                if (realtime == nullptr || realtime->protocolStack == nullptr) {
                  return grpc::Status(
                      grpc::StatusCode::FAILED_PRECONDITION,
                      "IEC61850保护动作发布会话不存在");
                }
                ProtocolGoosePublishCommand command;
                command.publisherId = publisherId;
                // 保留旧字段，便于旧的适配器实现继续识别动作编号。
                command.subscriptionId = publisherId;
                command.stateChanged = stateChanged;
                command.values = values;
                return realtime->protocolStack->PublishGoose(
                    realtime->connName, command);
              },
              runtime.realtimeRuntime->runtimePolicy);
      status = runtime.realtimeRuntime->protectionActionDispatcher->Start();
      if (!status.ok()) {
        LOG_ERROR("IEC61850保护动作发送线程启动失败: IED={}, 原因={}",
                  connName, status.error_message());
        const auto failedRealtimeRuntime = runtime.realtimeRuntime;
        if (failedRealtimeRuntime->bus != nullptr) {
          failedRealtimeRuntime->bus->Invalidate();
        }
        runtime.realtimeRuntime.reset();
        runtime.state = IEC61850Proto::IED_STATE_ERROR;
        runtime.lastError = status.error_message();
        StopRealtimeWorkers(failedRealtimeRuntime);
        return status;
      }
    }
    std::size_t producerCount = 0;
    for (const auto& subscription : plan.gooseSubscriptions) {
      producerCount += subscription.endpoints.size();
    }
    for (const auto& stream : plan.svStreams) {
      producerCount += stream.endpoints.size();
    }
    producerCount = std::max<std::size_t>(producerCount, 1);
    runtime.realtimeRuntime->bus =
        std::make_shared<RealtimeSignalBus>(producerCount, 1024,
                                            sessionGeneration);
    runtime.realtimeRuntime->producers.reserve(producerCount);
    for (std::size_t index = 0; index < producerCount; ++index) {
      runtime.realtimeRuntime->producers.emplace_back(
          runtime.realtimeRuntime->bus->producer(index));
    }
    std::size_t nextProducerIndex = 0;
    for (const auto& subscription : plan.gooseSubscriptions) {
      auto& route = runtime.realtimeRuntime->gooseRoutes.emplace_back();
      route.subscriptionId = subscription.subscriptionId;
      for (const auto& member : subscription.members) {
        route.signalIds.push_back(member.signalId);
        route.valueTypes.push_back(
            member.valueType == IEC61850Proto::POINT_VALUE_TYPE_BOOL
                ? ProtocolRealtimeValueType::BOOLEAN
                : member.valueType == IEC61850Proto::POINT_VALUE_TYPE_INT64
                    ? ProtocolRealtimeValueType::INTEGER
                    : ProtocolRealtimeValueType::FLOATING);
      }
      for (const auto& endpoint : subscription.endpoints) {
        const auto channel = static_cast<std::size_t>(endpoint.channel);
        if (channel < route.producerIndices.size() &&
            nextProducerIndex < producerCount) {
          route.producerIndices[channel] = nextProducerIndex++;
        }
      }
    }
    if (iedConfig.enable_goose()) {
      std::vector<GooseRealtimeSubscriptionConfig> subscriptions;
      subscriptions.reserve(plan.gooseSubscriptions.size());
      for (const auto& subscription : plan.gooseSubscriptions) {
        GooseRealtimeSubscriptionConfig config;
        config.subscriptionId = subscription.subscriptionId;
        config.gocbRef = subscription.controlRef;
        config.dataSetRef = subscription.dataSetRef;
        config.goId = subscription.goId;
        config.configRevision = subscription.configRevision;
        for (const auto& endpoint : subscription.endpoints) {
          const auto channel = static_cast<std::size_t>(endpoint.channel);
          if (channel < config.appIds.size()) {
            config.appIds[channel] = endpoint.appId;
          }
        }
        for (const auto& member : subscription.members) {
          config.signalIds.push_back(member.signalId);
          config.valueTypes.push_back(
              member.valueType == IEC61850Proto::POINT_VALUE_TYPE_BOOL
                  ? ProtocolRealtimeValueType::BOOLEAN
                  : member.valueType == IEC61850Proto::POINT_VALUE_TYPE_INT64
                      ? ProtocolRealtimeValueType::INTEGER
                      : ProtocolRealtimeValueType::FLOATING);
        }
        subscriptions.emplace_back(std::move(config));
      }
      runtime.realtimeRuntime->gooseEngine =
          std::make_shared<GooseRealtimeEngine>(std::move(subscriptions));
    }
    if (iedConfig.enable_sv()) {
      std::vector<SvRealtimeSubscriptionConfig> subscriptions;
      std::vector<SvMathStreamPlan> mathPlans;
      subscriptions.reserve(plan.svStreams.size());
      mathPlans.reserve(plan.svStreams.size());
      for (const auto& stream : plan.svStreams) {
        auto& route = runtime.realtimeRuntime->svRoutes.emplace_back();
        route.streamId = stream.streamId;
        SvRealtimeSubscriptionConfig config;
        config.streamId = stream.streamId;
        config.svId = stream.svId;
        config.configRevision = stream.configRevision;
        config.expectedAsduCount = stream.nofAsdu;
        config.nominalFrequencyHz = stream.nominalFrequencyHz;
        config.sampleRateHz = static_cast<double>(stream.sampleRate) *
                              config.nominalFrequencyHz;
        if (stream.sampleRate <= 4096) {
          config.sampleWindowSize = stream.sampleRate;
        }
        LOG_INFO(
            "IEC61850配置SV数值采样窗口: IED={}, 流={}, 每周波采样数={}, 绝对采样率={}, 额定频率={}, 窗口={}",
            iedConfig.conn_name(), stream.streamId, stream.sampleRate,
            config.sampleRateHz, config.nominalFrequencyHz,
            config.sampleWindowSize);
        for (const auto& member : stream.members) {
          route.signalIds.push_back(member.signalId);
          const auto valueType =
              member.valueType == IEC61850Proto::POINT_VALUE_TYPE_BOOL
                  ? ProtocolRealtimeValueType::BOOLEAN
                  : member.valueType == IEC61850Proto::POINT_VALUE_TYPE_INT64
                        ? ProtocolRealtimeValueType::INTEGER
                        : ProtocolRealtimeValueType::FLOATING;
          route.valueTypes.push_back(valueType);
          config.valueTypes.push_back(valueType);
        }
        for (const auto& endpoint : stream.endpoints) {
          const auto channel = static_cast<std::size_t>(endpoint.channel);
          if (channel < config.appIds.size()) {
            config.appIds[channel] = endpoint.appId;
          }
          if (channel < route.producerIndices.size() &&
              nextProducerIndex < producerCount) {
            route.producerIndices[channel] = nextProducerIndex++;
          }
        }
        subscriptions.emplace_back(std::move(config));

        if (stream.nofAsdu != 1) {
          if (!stream.derivedMembers.empty()) {
            LOG_WARNING(
                "IEC61850首期SV数学计算跳过多ASDU流: IED={}, 流={}, ASDU数量={}",
                iedConfig.conn_name(), stream.streamId, stream.nofAsdu);
          }
          continue;
        }
        if (stream.sampleRate > 4096) {
          LOG_WARNING(
              "IEC61850首期SV数学计算跳过超出窗口上限的流: IED={}, 流={}, 每周波采样数={}",
              iedConfig.conn_name(), stream.streamId, stream.sampleRate);
          continue;
        }
        if (stream.derivedMembers.empty()) {
          continue;
        }
        SvMathStreamPlan mathPlan;
        mathPlan.streamId = stream.streamId;
        mathPlan.samplesPerCycle = stream.sampleRate;
        mathPlan.expectedAsduCount = stream.nofAsdu;
        mathPlan.nominalFrequencyHz = config.nominalFrequencyHz;
        mathPlan.members.reserve(stream.derivedMembers.size());
        for (const auto& member : stream.derivedMembers) {
          mathPlan.members.push_back({.inputSignalId = member.inputSignalId,
                                      .rmsSignalId = member.rmsSignalId});
        }
        mathPlans.emplace_back(std::move(mathPlan));
      }
      runtime.realtimeRuntime->svEngine =
          std::make_shared<SvRealtimeEngine>(std::move(subscriptions));
      if (!mathPlans.empty()) {
        runtime.realtimeRuntime->svMathEngine =
            std::make_shared<SvMathEngine>(std::move(mathPlans),
                                            sessionGeneration);
      }
    }
    const auto abortRealtimeThreadStart =
        [&](const grpc::Status& failure) -> grpc::Status {
      const auto failedRealtimeRuntime = runtime.realtimeRuntime;
      if (failedRealtimeRuntime != nullptr &&
          failedRealtimeRuntime->bus != nullptr) {
        failedRealtimeRuntime->bus->Invalidate();
      }
      runtime.realtimeRuntime.reset();
      runtime.state = IEC61850Proto::IED_STATE_ERROR;
      runtime.protocolSessionActive = false;
      runtime.lastError = failure.error_message();
      runtime.channels.clear();
      lock.unlock();
      StopRealtimeWorkers(failedRealtimeRuntime);
      return failure;
    };
    if (runtime.realtimeRuntime->gooseEngine != nullptr) {
      const std::weak_ptr<RealtimeRuntime> weakRealtime =
          runtime.realtimeRuntime;
      std::size_t maxGooseValueCount = 0;
      for (const auto& route : runtime.realtimeRuntime->gooseRoutes) {
        maxGooseValueCount =
            std::max(maxGooseValueCount, route.signalIds.size());
      }
      const auto timedOutValues =
          std::make_shared<std::vector<ProtocolRealtimeValue>>(
              maxGooseValueCount);
      std::jthread worker;
      status = StartThreadWithRuntimePolicy(
          &worker, runtime.realtimeRuntime->runtimePolicy,
          [weakRealtime, timedOutValues](std::stop_token stopToken) {
            while (!stopToken.stop_requested()) {
              const auto realtime = weakRealtime.lock();
              if (realtime == nullptr || realtime->gooseEngine == nullptr) {
                return;
              }
              std::size_t routeIndex = 0;
              const auto nowNs = std::chrono::duration_cast<
                                     std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now()
                                         .time_since_epoch())
                                     .count();
              std::size_t timedOutValueCount = 0;
              const auto result = realtime->gooseEngine->CheckTimeout(
                  nowNs, &routeIndex,
                  std::span<ProtocolRealtimeValue>(*timedOutValues),
                  &timedOutValueCount);
              if (result == GooseRealtimeProcessResult::TIMED_OUT &&
                  routeIndex < realtime->gooseRoutes.size()) {
                realtime->gooseTimeouts.fetch_add(1,
                                                   std::memory_order_relaxed);
                const auto& route = realtime->gooseRoutes[routeIndex];
                std::size_t producerIndex =
                    std::numeric_limits<std::size_t>::max();
                for (const auto candidate : route.producerIndices) {
                  if (candidate < realtime->producers.size()) {
                    producerIndex = candidate;
                    break;
                  }
                }
                if (producerIndex < realtime->producers.size()) {
                  if (timedOutValueCount > timedOutValues->size()) {
                    LOG_ERROR(
                        "IEC61850 GOOSE超时快照缓冲不足: IED={}, 订阅={}, 成员数={}",
                        realtime->connName,
                        route.subscriptionId, timedOutValueCount);
                    continue;
                  }
                  const auto values = std::span<const ProtocolRealtimeValue>(
                      timedOutValues->data(), timedOutValueCount);
                  for (std::size_t index = 0;
                       index < values.size() && index < route.signalIds.size();
                       ++index) {
                    RealtimeSignalUpdate update;
                    update.signalId = route.signalIds[index];
                    update.sessionGeneration = realtime->sessionGeneration;
                    update.source = RealtimeSignalSource::GOOSE;
                    update.valueType = static_cast<RealtimeSignalValueType>(
                        values[index].valueType);
                    update.channel = RealtimeNetworkChannel::UNSPECIFIED;
                    update.qualityBits =
                        values[index].qualityBits |
                        kRealtimeCommunicationInvalid;
                    update.timestampNs = nowNs;
                    switch (values[index].valueType) {
                      case ProtocolRealtimeValueType::BOOLEAN:
                        update.value.booleanValue =
                            values[index].value.booleanValue;
                        break;
                      case ProtocolRealtimeValueType::INTEGER:
                        update.value.integerValue =
                            values[index].value.integerValue;
                        break;
                      case ProtocolRealtimeValueType::FLOATING:
                        update.value.floatingValue =
                            values[index].value.floatingValue;
                        break;
                    }
                    if (!realtime->producers[producerIndex].TryPublish(update)) {
                      LOG_WARNING(
                          "IEC61850 GOOSE超时质量更新未进入实时总线: 信号={}, 会话代际={}",
                          update.signalId, realtime->sessionGeneration);
                    }
                  }
                }
              } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
              }
            }
          },
          &runtime.realtimeRuntime->gooseTimeoutRuntimeState);
      if (!status.ok()) {
        return abortRealtimeThreadStart(status);
      }
      runtime.realtimeRuntime->gooseTimeoutWorker = std::move(worker);
    }
    {
      const std::weak_ptr<RealtimeRuntime> weakRealtime =
          runtime.realtimeRuntime;
      std::jthread worker;
      status = StartThreadWithRuntimePolicy(
          &worker, runtime.realtimeRuntime->runtimePolicy,
          [weakRealtime](std::stop_token stopToken) {
            RealtimeSignalUpdate update;
            std::array<ProtectionAction, 16> protectionActions;
            std::array<ProtectionActionCompletion, 32> completions;
            std::vector<RealtimeSignalUpdate> svMathOutputs;
            bool svMathBufferReady = false;
            while (!stopToken.stop_requested()) {
              const auto realtime = weakRealtime.lock();
              if (realtime == nullptr || realtime->bus == nullptr) {
                return;
              }
              if (!svMathBufferReady) {
                if (realtime->svMathEngine != nullptr) {
                  svMathOutputs.resize(
                      realtime->svMathEngine->maxOutputCount());
                }
                svMathBufferReady = true;
              }
              if (realtime->protectionActionDispatcher != nullptr &&
                  realtime->protectionEngine != nullptr) {
                while (true) {
                  const auto completionCount =
                      realtime->protectionActionDispatcher
                          ->DrainCompletions(completions);
                  if (completionCount == 0) {
                    break;
                  }
                  for (std::size_t index = 0; index < completionCount;
                       ++index) {
                    realtime->protectionEngine->CompleteAction(
                        completions[index].ruleIndex,
                        completions[index].asserted,
                        completions[index].success);
                  }
                }
              }
              const auto processRealtimeUpdate =
                  [&realtime](const RealtimeSignalUpdate& item) {
                    if (realtime->signalProcessor != nullptr) {
                      realtime->signalProcessor->Process(item);
                    }
                    if (realtime->protectionEngine != nullptr) {
                      const auto nowNs = item.timestampNs > 0
                                             ? item.timestampNs
                                             : std::chrono::duration_cast<
                                                   std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now()
                                                       .time_since_epoch())
                                                   .count();
                      realtime->protectionEngine->Process(item, nowNs);
                    }
                  };
              bool consumed = false;
              while (realtime->bus->TryConsume(&update)) {
                consumed = true;
                processRealtimeUpdate(update);
                if (realtime->svMathEngine != nullptr &&
                    !svMathOutputs.empty()) {
                  const auto nowNs = update.timestampNs > 0
                                         ? update.timestampNs
                                         : std::chrono::duration_cast<
                                               std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now()
                                                   .time_since_epoch())
                                               .count();
                  const auto outputCount = realtime->svMathEngine->Process(
                      update, nowNs, svMathOutputs);
                  for (std::size_t index = 0; index < outputCount; ++index) {
                    processRealtimeUpdate(svMathOutputs[index]);
                  }
                }
                realtime->realtimeUpdatesConsumed.fetch_add(
                    1, std::memory_order_relaxed);
              }
              if (realtime->protectionEngine != nullptr) {
                const auto nowNs = std::chrono::duration_cast<
                                       std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now()
                                           .time_since_epoch())
                                       .count();
                realtime->protectionEngine->Tick(nowNs);
                while (true) {
                  const auto actionCount =
                      realtime->protectionEngine->DrainActions(protectionActions);
                  if (actionCount == 0) {
                    break;
                  }
                  for (std::size_t index = 0; index < actionCount; ++index) {
                    if (realtime->protectionActionDispatcher == nullptr ||
                        !realtime->protectionActionDispatcher->TryEnqueue(
                            protectionActions[index])) {
                      realtime->protectionEngine->CompleteAction(
                          protectionActions[index].ruleIndex,
                          protectionActions[index].asserted, false);
                      LOG_WARNING(
                          "IEC61850保护动作发送队列无法接收动作: IED={}, 订阅={}",
                          realtime->connName,
                          protectionActions[index].outputSubscriptionId);
                      continue;
                    }
                  }
                }
              }
              if (!consumed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
              }
            }
          },
          &runtime.realtimeRuntime->realtimeConsumerRuntimeState);
      if (!status.ok()) {
        return abortRealtimeThreadStart(status);
      }
      runtime.realtimeRuntime->realtimeConsumerWorker = std::move(worker);
    }
    realtimeRuntime = runtime.realtimeRuntime;
    runtime.channels.clear();
    for (const auto& channel : iedConfig.channels()) {
      auto& channelRuntime =
          runtime.channels[static_cast<int>(channel.channel())];
      if (!channel.enabled()) {
        channelRuntime.state = IEC61850Proto::CHANNEL_STATE_DISABLED;
      } else if (iedConfig.enable_mms()) {
        channelRuntime.state = IEC61850Proto::CHANNEL_STATE_CONNECTING;
      } else {
        channelRuntime.state = IEC61850Proto::CHANNEL_STATE_UNSPECIFIED;
      }
      channelRuntime.lastError.clear();
    }
    stack = protocolStack_;
  }

  ProtocolEventCallbacks callbacks;
  if (iedConfig.enable_mms()) {
    MmsPublishConfig publishConfig;
    publishConfig.connName = connName;
    publishConfig.connId = connId;
    publishConfig.mappings = std::move(mappings);
    publishConfig.queueCapacity = iedConfig.mms_event_queue_capacity();
    publishConfig.batchSize = iedConfig.publish_batch_size();
    publishConfig.batchWindow =
        std::chrono::milliseconds(iedConfig.publish_batch_window_ms());
    uint64_t activationToken = 0;
    status = mmsPipeline_.ConfigureIed(std::move(publishConfig),
                                      &activationToken);
    if (!status.ok()) {
      std::shared_ptr<RealtimeRuntime> failedRealtimeRuntime;
      {
        std::lock_guard lock(mutex_);
        auto& runtime = runtimeByConnName_[connName];
        AdvanceSessionGeneration(&runtime.sessionGeneration);
        runtime.state = IEC61850Proto::IED_STATE_ERROR;
        runtime.protocolSessionActive = false;
        runtime.mmsPipelineActive = false;
        if (runtime.realtimeRuntime != nullptr &&
            runtime.realtimeRuntime->bus != nullptr) {
          runtime.realtimeRuntime->bus->Invalidate();
        }
        failedRealtimeRuntime = runtime.realtimeRuntime;
        runtime.realtimeRuntime.reset();
        runtime.channels.clear();
        runtime.lastError = status.error_message();
      }
      StopRealtimeWorkers(failedRealtimeRuntime);
      return status;
    }
    {
      std::lock_guard lock(mutex_);
      runtimeByConnName_[connName].mmsPipelineActive = true;
    }
    callbacks.onMmsReport =
        mmsPipeline_.MakeReportCallback(connName, activationToken);
  }
  if (iedConfig.enable_goose() || iedConfig.enable_sv()) {
    callbacks.onGooseFrame =
        [realtimeRuntime, sessionGeneration](ProtocolGooseFrameView frame) {
          Manager::PublishGooseFrame(realtimeRuntime, sessionGeneration, frame);
        };
    callbacks.onSvFrame =
        [realtimeRuntime, sessionGeneration](ProtocolSvFrameView frame) {
          Manager::PublishSvFrame(realtimeRuntime, sessionGeneration, frame);
        };
  }
  if (iedConfig.enable_mms()) {
    std::weak_ptr<CallbackGate> weakGate = callbackGate_;
    callbacks.onMmsConnection =
        [weakGate, connName,
         sessionGeneration](MmsConnectionEvent event) mutable {
          const auto gate = weakGate.lock();
          if (!gate) {
            return;
          }
          std::lock_guard lock(gate->mutex);
          if (gate->owner != nullptr) {
            gate->owner->HandleMmsConnectionEvent(
                connName, sessionGeneration, std::move(event));
          }
        };
  }

  LOG_INFO(
      "IEC61850已生成单IED协议启动计划: IED={}, SCL设备={}, AccessPoint={}, 网络绑定数量={}, ConnectedAP数量={}, GOOSE订阅数量={}, GOOSE发布数量={}, SV流数量={}, 实时信号数量={}, DataSet数量={}, ReportControl数量={}",
      connName, plan.ied.name(), iedConfig.access_point(),
      plan.networkBindings.size(), plan.connectedAccessPoints.size(),
      plan.gooseSubscriptions.size(), plan.goosePublishers.size(),
      plan.svStreams.size(),
      plan.realtimeSignals.size(),
      plan.ied.data_sets_size(),
      plan.ied.report_controls_size());
  try {
    status = stack->StartIed(std::move(plan), std::move(callbacks));
  } catch (const std::exception& exception) {
    LOG_ERROR("IEC61850启动时协议栈启动接口发生异常: IED={}, 异常信息={}",
              connName, exception.what());
    status = ProtocolStackExceptionStatus("启动");
  } catch (...) {
    LOG_ERROR("IEC61850启动时协议栈启动接口发生未知异常: IED={}",
              connName);
    status = ProtocolStackExceptionStatus("启动");
  }
  grpc::Status mmsCleanupStatus;
  if (!status.ok() && iedConfig.enable_mms()) {
    mmsCleanupStatus = mmsPipeline_.DeactivateIed(connName);
    if (!mmsCleanupStatus.ok()) {
      LOG_ERROR("IEC61850启动失败后停止MMS发布入口也失败: IED={}, 原因={}",
                connName, mmsCleanupStatus.error_message());
    }
  }
  std::lock_guard lock(mutex_);
  auto& runtime = runtimeByConnName_[connName];
  if (!status.ok()) {
    if (runtime.sessionGeneration == sessionGeneration) {
      AdvanceSessionGeneration(&runtime.sessionGeneration);
    }
    runtime.state = IEC61850Proto::IED_STATE_ERROR;
    runtime.activeChannel = IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
    runtime.protocolSessionActive = false;
    runtime.mmsPipelineActive =
        iedConfig.enable_mms() && !mmsCleanupStatus.ok();
    if (runtime.realtimeRuntime != nullptr &&
        runtime.realtimeRuntime->bus != nullptr) {
      runtime.realtimeRuntime->bus->Invalidate();
      runtime.realtimeRuntime->gooseTimeoutWorker.request_stop();
      runtime.realtimeRuntime->realtimeConsumerWorker.request_stop();
    }
    const auto failedRealtimeRuntime = runtime.realtimeRuntime;
    StopRealtimeWorkers(failedRealtimeRuntime);
    if (!runtime.mmsPipelineActive) {
      runtime.realtimeRuntime.reset();
    }
    runtime.channels.clear();
    runtime.lastError = status.error_message();
    return status;
  }
  runtime.protocolSessionActive = true;
  if (!iedConfig.enable_mms()) {
    runtime.state = IEC61850Proto::IED_STATE_RUNNING;
    runtime.activeChannel = IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  }
  LOG_INFO("IEC61850已启动IED通信功能: IED={}", connName);
  return grpc::Status::OK;
}

grpc::Status Manager::StopIed(const std::string& connName) {
  auto status = ValidateName(connName, "conn_name");
  if (!status.ok()) {
    return status;
  }
  std::lock_guard operationLock(operationMutex_);
  return StopIedOperation(connName, true);
}

grpc::Status Manager::ReadMms(const std::string& connName,
                              const MmsReadRequest& request,
                              MmsReadResponse* response) {
  auto status = ValidateName(connName, "conn_name");
  if (!status.ok()) {
    return status;
  }
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Read响应输出不能为空");
  }
  response->items.clear();
  if (request.variables.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Read变量列表不能为空");
  }

  std::shared_ptr<ProtocolStackAdapter> stack;
  {
    std::lock_guard lock(mutex_);
    if (shuttingDown_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850模块正在停止，不能执行MMS Read");
    }
    const auto* persisted = FindIed(config_, connName);
    if (persisted == nullptr) {
      return NotFound("IED连接不存在");
    }
    if (persisted->pending_delete()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED配置处于待删除状态，不能执行MMS Read");
    }
    if (!persisted->config().enable_mms()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED未启用MMS，不能执行MMS Read");
    }
    const auto runtimeIt = runtimeByConnName_.find(connName);
    if (runtimeIt == runtimeByConnName_.end() ||
        !runtimeIt->second.protocolSessionActive) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED通信功能尚未启动，不能执行MMS Read");
    }
    if (runtimeIt->second.state != IEC61850Proto::IED_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED MMS会话尚未就绪，不能执行MMS Read");
    }
    stack = protocolStack_;
  }
  if (stack == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850协议栈未配置，不能执行MMS Read");
  }

  LOG_INFO("IEC61850收到MMS Read控制请求: IED={}, 变量数={}", connName,
           request.variables.size());
  try {
    status = stack->ReadMms(connName, request, response);
  } catch (const std::exception& exception) {
    response->items.clear();
    LOG_ERROR("IEC61850 MMS Read协议栈调用发生异常: IED={}, 异常信息={}",
              connName, exception.what());
    return ProtocolStackExceptionStatus("MMS Read");
  } catch (...) {
    response->items.clear();
    LOG_ERROR("IEC61850 MMS Read协议栈调用发生未知异常: IED={}", connName);
    return ProtocolStackExceptionStatus("MMS Read");
  }
  if (!status.ok()) {
    response->items.clear();
    LOG_WARNING("IEC61850 MMS Read控制请求失败: IED={}, 原因={}", connName,
                status.error_message());
  } else {
    LOG_INFO("IEC61850 MMS Read控制请求完成: IED={}, 结果数={}", connName,
             response->items.size());
  }
  return status;
}

grpc::Status Manager::WriteMms(const std::string& connName,
                               const MmsWriteRequest& request,
                               MmsWriteResponse* response) {
  auto status = ValidateName(connName, "conn_name");
  if (!status.ok()) {
    return status;
  }
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Write响应输出不能为空");
  }
  response->items.clear();
  if (request.items.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Write变量列表不能为空");
  }

  std::shared_ptr<ProtocolStackAdapter> stack;
  {
    std::lock_guard lock(mutex_);
    if (shuttingDown_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850模块正在停止，不能执行MMS Write");
    }
    const auto* persisted = FindIed(config_, connName);
    if (persisted == nullptr) {
      return NotFound("IED连接不存在");
    }
    if (persisted->pending_delete()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED配置处于待删除状态，不能执行MMS Write");
    }
    if (!persisted->config().enable_mms()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED未启用MMS，不能执行MMS Write");
    }
    const auto runtimeIt = runtimeByConnName_.find(connName);
    if (runtimeIt == runtimeByConnName_.end() ||
        !runtimeIt->second.protocolSessionActive) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED通信功能尚未启动，不能执行MMS Write");
    }
    if (runtimeIt->second.state != IEC61850Proto::IED_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED MMS会话尚未就绪，不能执行MMS Write");
    }
    stack = protocolStack_;
  }
  if (stack == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850协议栈未配置，不能执行MMS Write");
  }

  LOG_INFO("IEC61850收到MMS Write控制请求: IED={}, 变量数={}", connName,
           request.items.size());
  try {
    status = stack->WriteMms(connName, request, response);
  } catch (const std::exception& exception) {
    response->items.clear();
    LOG_ERROR("IEC61850 MMS Write协议栈调用发生异常: IED={}, 异常信息={}",
              connName, exception.what());
    return ProtocolStackExceptionStatus("MMS Write");
  } catch (...) {
    response->items.clear();
    LOG_ERROR("IEC61850 MMS Write协议栈调用发生未知异常: IED={}", connName);
    return ProtocolStackExceptionStatus("MMS Write");
  }
  if (!status.ok()) {
    response->items.clear();
    LOG_WARNING("IEC61850 MMS Write控制请求失败: IED={}, 原因={}", connName,
                status.error_message());
  } else {
    LOG_INFO("IEC61850 MMS Write控制请求完成: IED={}, 结果数={}", connName,
             response->items.size());
  }
  return status;
}

grpc::Status Manager::SelectMmsControl(const std::string& connName,
                                       const MmsObjectName& controlObject,
                                       MmsReadResponse* response) {
  auto status = ValidateName(connName, "conn_name");
  if (!status.ok()) {
    return status;
  }
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS SBO选择响应输出不能为空");
  }
  response->items.clear();
  MmsReadRequest request;
  status = BuildMmsControlSelectRequest(controlObject, &request);
  if (!status.ok()) {
    return status;
  }

  std::shared_ptr<ProtocolStackAdapter> stack;
  {
    std::lock_guard lock(mutex_);
    if (shuttingDown_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850模块正在停止，不能执行MMS SBO选择");
    }
    const auto* persisted = FindIed(config_, connName);
    if (persisted == nullptr) {
      return NotFound("IED连接不存在");
    }
    if (persisted->pending_delete()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED配置处于待删除状态，不能执行MMS SBO选择");
    }
    if (!persisted->config().enable_mms()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED未启用MMS，不能执行MMS SBO选择");
    }
    const auto runtimeIt = runtimeByConnName_.find(connName);
    if (runtimeIt == runtimeByConnName_.end() ||
        !runtimeIt->second.protocolSessionActive) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED通信功能尚未启动，不能执行MMS SBO选择");
    }
    if (runtimeIt->second.state != IEC61850Proto::IED_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED MMS会话尚未就绪，不能执行MMS SBO选择");
    }
    stack = protocolStack_;
  }
  if (stack == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850协议栈未配置，不能执行MMS SBO选择");
  }

  LOG_INFO("IEC61850收到MMS SBO选择请求: IED={}, 对象={}", connName,
           controlObject.identifier);
  try {
    status = stack->SelectMmsControl(connName, controlObject, response);
  } catch (const std::exception& exception) {
    response->items.clear();
    LOG_ERROR("IEC61850 MMS SBO选择协议栈调用发生异常: IED={}, 异常信息={}",
              connName, exception.what());
    return ProtocolStackExceptionStatus("MMS SBO选择");
  } catch (...) {
    response->items.clear();
    LOG_ERROR("IEC61850 MMS SBO选择协议栈调用发生未知异常: IED={}",
              connName);
    return ProtocolStackExceptionStatus("MMS SBO选择");
  }
  if (!status.ok()) {
    response->items.clear();
    LOG_WARNING("IEC61850 MMS SBO选择失败: IED={}, 原因={}", connName,
                status.error_message());
  } else {
    LOG_INFO("IEC61850 MMS SBO选择完成: IED={}, 结果数={}", connName,
             response->items.size());
  }
  return status;
}

grpc::Status Manager::WriteMmsControl(const std::string& connName,
                                      const MmsControlCommand& command,
                                      MmsWriteResponse* response) {
  auto status = ValidateName(connName, "conn_name");
  if (!status.ok()) {
    return status;
  }
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS控制Write响应输出不能为空");
  }
  response->items.clear();
  MmsWriteRequest request;
  status = BuildMmsControlWriteRequest(command, &request);
  if (!status.ok()) {
    return status;
  }

  std::shared_ptr<ProtocolStackAdapter> stack;
  {
    std::lock_guard lock(mutex_);
    if (shuttingDown_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850模块正在停止，不能执行MMS控制Write");
    }
    const auto* persisted = FindIed(config_, connName);
    if (persisted == nullptr) {
      return NotFound("IED连接不存在");
    }
    if (persisted->pending_delete()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED配置处于待删除状态，不能执行MMS控制Write");
    }
    if (!persisted->config().enable_mms()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED未启用MMS，不能执行MMS控制Write");
    }
    const auto runtimeIt = runtimeByConnName_.find(connName);
    if (runtimeIt == runtimeByConnName_.end() ||
        !runtimeIt->second.protocolSessionActive) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED通信功能尚未启动，不能执行MMS控制Write");
    }
    if (runtimeIt->second.state != IEC61850Proto::IED_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED MMS会话尚未就绪，不能执行MMS控制Write");
    }
    stack = protocolStack_;
  }
  if (stack == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850协议栈未配置，不能执行MMS控制Write");
  }

  LOG_INFO("IEC61850收到MMS控制Write请求: IED={}, 对象={}, 操作={}",
           connName, command.controlObject.identifier,
           static_cast<int>(command.operation));
  try {
    status = stack->WriteMmsControl(connName, command, response);
  } catch (const std::exception& exception) {
    response->items.clear();
    LOG_ERROR("IEC61850 MMS控制Write协议栈调用发生异常: IED={}, 异常信息={}",
              connName, exception.what());
    return ProtocolStackExceptionStatus("MMS控制Write");
  } catch (...) {
    response->items.clear();
    LOG_ERROR("IEC61850 MMS控制Write协议栈调用发生未知异常: IED={}",
              connName);
    return ProtocolStackExceptionStatus("MMS控制Write");
  }
  if (!status.ok()) {
    response->items.clear();
    LOG_WARNING("IEC61850 MMS控制Write失败: IED={}, 原因={}", connName,
                status.error_message());
  } else {
    LOG_INFO("IEC61850 MMS控制Write完成: IED={}, 结果数={}", connName,
             response->items.size());
  }
  return status;
}

grpc::Status Manager::ReadSettingGroupStatus(
    const std::string& connName, const MmsSettingGroupPlan& plan,
    MmsSettingGroupStatus* status) {
  if (status == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 SGCB状态输出不能为空");
  }
  auto stack = protocolStack_;
  if (stack == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850协议栈未配置，不能读取SGCB状态");
  }
  LOG_INFO("IEC61850收到SGCB状态读取请求: IED={}", connName);
  return stack->ReadSettingGroupStatus(connName, plan, status);
}

grpc::Status Manager::SelectSettingGroup(const std::string& connName,
                                         const MmsSettingGroupPlan& plan,
                                         std::uint32_t group) {
  auto stack = protocolStack_;
  if (stack == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850协议栈未配置，不能选择SGCB组");
  }
  LOG_INFO("IEC61850收到SGCB组选择请求: IED={}, 组号={}", connName, group);
  return stack->SelectSettingGroup(connName, plan, group);
}

grpc::Status Manager::ConfirmSettingGroupEdit(
    const std::string& connName, const MmsSettingGroupPlan& plan) {
  auto stack = protocolStack_;
  if (stack == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850协议栈未配置，不能确认SGCB修改");
  }
  LOG_INFO("IEC61850收到SGCB修改确认请求: IED={}", connName);
  return stack->ConfirmSettingGroupEdit(connName, plan);
}

grpc::Status Manager::CancelSettingGroupEdit(
    const std::string& connName, const MmsSettingGroupPlan& plan) {
  auto stack = protocolStack_;
  if (stack == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850协议栈未配置，不能取消SGCB修改");
  }
  LOG_INFO("IEC61850收到SGCB修改取消请求: IED={}", connName);
  return stack->CancelSettingGroupEdit(connName, plan);
}

grpc::Status Manager::ActivateSettingGroup(const std::string& connName,
                                           const MmsSettingGroupPlan& plan,
                                           std::uint32_t group) {
  auto stack = protocolStack_;
  if (stack == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850协议栈未配置，不能激活SGCB组");
  }
  LOG_INFO("IEC61850收到SGCB组激活请求: IED={}, 组号={}", connName, group);
  return stack->ActivateSettingGroup(connName, plan, group);
}

grpc::Status Manager::ExecuteDataCenterCommand(
    const DataCenterProto::ExecuteCommandRequest& request,
    DataCenterProto::ExecuteCommandResponse* response,
    std::shared_ptr<std::atomic_bool> cancellation) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850同步命令响应不能为空");
  }
  response->Clear();
  if (IsCancellationRequested(cancellation)) {
    LOG_WARNING("IEC61850同步MMS控制在进入协议栈前已取消");
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850同步MMS控制已取消");
  }
  // 与Start/Stop/Shutdown共用生命周期锁，禁止控制请求跨越IED停止或模块关闭。
  std::shared_lock operationLock(operationMutex_);
  {
    std::lock_guard lock(mutex_);
    if (shuttingDown_) {
      LOG_WARNING("IEC61850模块关闭期间拒绝同步MMS控制命令");
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850模块正在关闭");
    }
  }
  if (!request.has_dst()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "dst不能为空");
  }
  if (request.dst().conn_id() == 0 && request.dst().conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "dst.conn_id和dst.conn_name不能同时为空");
  }
  if (request.dst().tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "dst.tag不能为空");
  }
  if (!request.dst().module_name().empty() &&
      request.dst().module_name() != "IEC61850") {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "dst.module_name不是IEC61850");
  }
  if (request.value().kind_case() ==
      DataCenterProto::PointValue::KIND_NOT_SET) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "value不能为空");
  }
  if (request.quality() == DataCenterProto::QUALITY_BAD ||
      request.quality() == DataCenterProto::QUALITY_UNCERTAIN) {
    *response->mutable_dst() = request.dst();
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    response->set_reject_code(DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
    response->set_reason("IEC61850拒绝品质为坏或不确定的同步控制命令");
    return grpc::Status::OK;
  }

  *response->mutable_dst() = request.dst();
  IEC61850Proto::PointMapping mapping;
  std::string connName;
  std::shared_ptr<ProtocolStackAdapter> stack;
  {
    std::lock_guard lock(mutex_);
    const IEC61850Proto::PersistedIed* persisted = nullptr;
    if (!request.dst().conn_name().empty()) {
      persisted = FindIed(config_, request.dst().conn_name());
    } else {
      for (const auto& candidate : config_.ieds()) {
        if (candidate.conn_id() == request.dst().conn_id()) {
          persisted = &candidate;
          break;
        }
      }
    }
    if (persisted == nullptr) {
      response->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
      response->set_reason("未找到目的IEC61850 IED");
      return grpc::Status::OK;
    }
    connName = persisted->config().conn_name();
    if (request.dst().conn_id() != 0 &&
        request.dst().conn_id() != persisted->conn_id()) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
      response->set_reason("目的连接名称与conn_id不一致");
      return grpc::Status::OK;
    }
    response->mutable_dst()->set_conn_id(persisted->conn_id());
    response->mutable_dst()->set_conn_name(connName);
    response->mutable_dst()->set_module_name("IEC61850");
    if (!persisted->config().enable_mms()) {
      response->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
      response->set_reason("IEC61850 IED未启用MMS");
      return grpc::Status::OK;
    }
    if (persisted->pending_delete()) {
      LOG_WARNING("IEC61850 IED处于待删除状态，拒绝同步MMS控制: IED={}",
                  connName);
      response->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
      response->set_reason("IEC61850 IED处于待删除状态");
      return grpc::Status::OK;
    }
    const auto runtimeIt = runtimeByConnName_.find(connName);
    if (runtimeIt == runtimeByConnName_.end() ||
        !runtimeIt->second.protocolSessionActive ||
        runtimeIt->second.state != IEC61850Proto::IED_STATE_RUNNING) {
      response->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
      response->set_reason("IEC61850 IED MMS会话未运行或尚未就绪");
      return grpc::Status::OK;
    }
    const auto* mappings = FindMappings(config_, connName);
    if (mappings == nullptr) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
      response->set_reason("IEC61850目的IED没有点映射");
      return grpc::Status::OK;
    }
    const auto mappingIt = std::find_if(
        mappings->points().begin(), mappings->points().end(),
        [&](const auto& candidate) {
          return candidate.tag() == request.dst().tag();
        });
    if (mappingIt == mappings->points().end()) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
      response->set_reason("IEC61850目的点不存在");
      return grpc::Status::OK;
    }
    mapping = *mappingIt;
    if (mapping.source() != IEC61850Proto::POINT_SOURCE_MMS ||
        mapping.fc() != IEC61850Proto::FUNCTIONAL_CONSTRAINT_CO) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
      response->set_reason("IEC61850目的点不是MMS控制点");
      return grpc::Status::OK;
    }
    stack = protocolStack_;
  }

  const auto suffix = mapping.data_ref().ends_with(".ctlVal")
                          ? std::string_view(".ctlVal")
                          : mapping.data_ref().ends_with("$ctlVal")
                                ? std::string_view("$ctlVal")
                                : std::string_view{};
  if (suffix.empty()) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    response->set_reject_code(DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
    response->set_reason("IEC61850控制点data_ref必须以ctlVal结尾");
    return grpc::Status::OK;
  }
  MmsPointControlCommand command;
  command.valueType = mapping.value_type();
  command.scale = mapping.scale();
  command.offset = mapping.offset();
  if (request.timeout_ms() != 0) {
    command.requestTimeout = std::chrono::milliseconds(request.timeout_ms());
    LOG_DEBUG("IEC61850同步MMS控制采用调用方截止时间: IED={}, tag={}, 超时={}毫秒",
              connName, request.dst().tag(), request.timeout_ms());
  }
  // 命令和Manager都需要观察同一份共享取消状态；不能move走后再由
  // 协议栈返回后的成功门禁读取空指针。
  command.cancellation = cancellation;
  auto objectStatus = ParseMmsDomainObjectReference(
      std::string_view(mapping.data_ref()).substr(
          0, mapping.data_ref().size() - suffix.size()),
      &command.controlObject);
  if (!objectStatus.ok()) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    response->set_reject_code(DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
    response->set_reason(objectStatus.error_message());
    return grpc::Status::OK;
  }

  double requestedValue = 0.0;
  bool converted = false;
  switch (command.valueType) {
    case IEC61850Proto::POINT_VALUE_TYPE_BOOL:
      converted = PointValueToBool(request.value(), &command.boolValue);
      requestedValue = command.boolValue ? 1.0 : 0.0;
      break;
    case IEC61850Proto::POINT_VALUE_TYPE_INT64:
      converted = PointValueToInt64(request.value(), &command.intValue);
      requestedValue = static_cast<double>(command.intValue);
      break;
    case IEC61850Proto::POINT_VALUE_TYPE_DOUBLE:
      converted = PointValueToDouble(request.value(), &command.doubleValue);
      requestedValue = command.doubleValue;
      break;
    default:
      converted = false;
      break;
  }
  if (!converted) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
    response->set_reason("IEC61850控制点值类型不支持或与点映射不一致");
    return grpc::Status::OK;
  }
  response->set_requested_value(requestedValue);
  if (stack == nullptr) {
    response->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
    response->set_reason("IEC61850协议栈未配置");
    return grpc::Status::OK;
  }

  MmsWriteResponse writeResponse;
  grpc::Status status;
  try {
    status = stack->ExecuteMmsPointControl(connName, command, &writeResponse);
  } catch (const std::exception& exception) {
    LOG_ERROR("IEC61850同步MMS控制协议栈调用发生异常: IED={}, tag={}, 异常信息={}",
              connName, request.dst().tag(), exception.what());
    status = ProtocolStackExceptionStatus("同步MMS控制");
  } catch (...) {
    LOG_ERROR("IEC61850同步MMS控制协议栈调用发生未知异常: IED={}, tag={}",
              connName, request.dst().tag());
    status = ProtocolStackExceptionStatus("同步MMS控制");
  }
  if (IsCancellationRequested(cancellation)) {
    response->Clear();
    LOG_WARNING("IEC61850同步MMS控制执行期间收到取消，不返回成功结果: IED={}, tag={}",
                connName, request.dst().tag());
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850同步MMS控制已取消");
  }
  if (!status.ok()) {
    response->set_status(
        status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED
            ? DataCenterProto::COMMAND_TIMEOUT
            : status.error_code() == grpc::StatusCode::UNAVAILABLE ||
                      status.error_code() == grpc::StatusCode::NOT_FOUND
                  ? DataCenterProto::COMMAND_TARGET_UNAVAILABLE
                  : status.error_code() == grpc::StatusCode::INVALID_ARGUMENT ||
                            status.error_code() == grpc::StatusCode::FAILED_PRECONDITION
                        ? DataCenterProto::COMMAND_REJECTED
                        : DataCenterProto::COMMAND_INTERNAL_ERROR);
    response->set_reject_code(
        response->status() == DataCenterProto::COMMAND_REJECTED
            ? (status.error_code() == grpc::StatusCode::FAILED_PRECONDITION
                   ? DataCenterProto::COMMAND_REJECT_BAD_CONFIG
                   : DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT)
            : DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
    response->set_reason(status.error_message());
    LOG_WARNING("IEC61850同步MMS控制失败: IED={}, tag={}, status={}, 原因={}",
                connName, request.dst().tag(),
                static_cast<int>(response->status()), status.error_message());
    return grpc::Status::OK;
  }
  if (writeResponse.items.size() != 1 ||
      !writeResponse.items.front().success) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    response->set_reject_code(DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
    response->set_reason("IEC61850 MMS控制未返回单项成功确认");
    return grpc::Status::OK;
  }
  if (IsCancellationRequested(cancellation)) {
    response->Clear();
    LOG_WARNING("IEC61850同步MMS控制在确认后返回前收到取消: IED={}, tag={}",
                connName, request.dst().tag());
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850同步MMS控制已取消");
  }
  response->set_status(DataCenterProto::COMMAND_ACCEPTED);
  response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
  response->set_reason("IEC61850 MMS同步控制已执行");
  response->set_accepted_value(requestedValue);
  LOG_INFO("IEC61850同步MMS控制执行成功: IED={}, conn_id={}, tag={}, value={}, request_id={}",
           connName, response->dst().conn_id(), request.dst().tag(),
           requestedValue, request.request_id());
  return grpc::Status::OK;
}

grpc::Status Manager::PublishGoose(
    const std::string& connName, std::uint32_t subscriptionId,
    std::span<const ProtocolRealtimeValue> values, bool stateChanged) {
  if (connName.empty() || subscriptionId == 0 || values.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850内部GOOSE发布参数无效");
  }
  std::shared_ptr<ProtocolStackAdapter> stack;
  {
    std::lock_guard lock(mutex_);
    const auto runtimeIt = runtimeByConnName_.find(connName);
    if (runtimeIt == runtimeByConnName_.end() ||
        !runtimeIt->second.protocolSessionActive) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 IED通信功能尚未启动，不能发布GOOSE");
    }
    stack = protocolStack_;
  }
  if (stack == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850协议栈未配置");
  }
  ProtocolGoosePublishCommand command;
  command.subscriptionId = subscriptionId;
  command.stateChanged = stateChanged;
  command.values = values;
  try {
    return stack->PublishGoose(connName, command);
  } catch (const std::exception& exception) {
    LOG_ERROR("IEC61850内部GOOSE发布发生异常: IED={}, 异常信息={}",
              connName, exception.what());
    return grpc::Status(grpc::StatusCode::UNKNOWN,
                        "IEC61850内部GOOSE发布发生异常");
  } catch (...) {
    LOG_ERROR("IEC61850内部GOOSE发布发生未知异常: IED={}", connName);
    return grpc::Status(grpc::StatusCode::UNKNOWN,
                        "IEC61850内部GOOSE发布发生未知异常");
  }
}

grpc::Status Manager::StopIedOperation(const std::string& connName,
                                       bool persistDesiredState) {
  grpc::Status status;
  std::shared_ptr<ProtocolStackAdapter> stack;
  bool needsProtocolStop = false;
  bool needsMmsStop = false;
  bool stackStopped = false;
  bool mmsStopped = false;
  std::shared_ptr<RealtimeRuntime> realtimeRuntime;
  {
    std::lock_guard lock(mutex_);
    auto* persisted = FindIed(&config_, connName);
    if (persisted == nullptr) {
      return NotFound("IED连接不存在");
    }
    if (persistDesiredState) {
      IEC61850Proto::PersistedConfig candidate = config_;
      FindIed(&candidate, connName)->set_desired_running(false);
      status = SaveCandidateLocked(candidate);
      if (!status.ok()) {
        return status;
      }
    }
    auto& runtime = runtimeByConnName_[connName];
    needsProtocolStop = runtime.protocolSessionActive;
    needsMmsStop = runtime.mmsPipelineActive;
    if (needsProtocolStop || needsMmsStop) {
      runtime.state = IEC61850Proto::IED_STATE_STOPPING;
      AdvanceSessionGeneration(&runtime.sessionGeneration);
      if (runtime.realtimeRuntime != nullptr &&
          runtime.realtimeRuntime->bus != nullptr) {
        runtime.realtimeRuntime->bus->Invalidate();
      }
      realtimeRuntime = runtime.realtimeRuntime;
    }
    stack = protocolStack_;
  }
  // 先停止实时消费者及保护动作发送器，确保协议栈关闭前不会再有GOOSE发布。
  StopRealtimeWorkers(realtimeRuntime);
  grpc::Status mmsStatus;
  if (needsMmsStop) {
    mmsStatus = mmsPipeline_.DeactivateIed(connName);
    mmsStopped = mmsStatus.ok();
    if (!mmsStatus.ok()) {
      LOG_ERROR("IEC61850停止MMS报告入口失败: IED={}, 原因={}", connName,
                mmsStatus.error_message());
    }
  }
  grpc::Status stackStatus;
  if (needsProtocolStop) {
    try {
      stackStatus = stack->StopIed(connName);
    } catch (const std::exception& exception) {
      LOG_ERROR("IEC61850停止时协议栈停止接口发生异常: IED={}, 异常信息={}",
                connName, exception.what());
      stackStatus = ProtocolStackExceptionStatus("停止");
    } catch (...) {
      LOG_ERROR("IEC61850停止时协议栈停止接口发生未知异常: IED={}", connName);
      stackStatus = ProtocolStackExceptionStatus("停止");
    }
    stackStopped = stackStatus.ok();
    if (!stackStatus.ok()) {
      LOG_ERROR("IEC61850停止协议栈会话失败: IED={}, 原因={}", connName,
                stackStatus.error_message());
    }
  }
  status = !stackStatus.ok() ? stackStatus : mmsStatus;
  std::lock_guard lock(mutex_);
  auto& runtime = runtimeByConnName_[connName];
  if (stackStopped || !needsProtocolStop) {
    runtime.protocolSessionActive = false;
    runtime.activeChannel = IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
    runtime.channels.clear();
  }
  if (mmsStopped || !needsMmsStop) {
    runtime.mmsPipelineActive = false;
  }
  if (runtime.realtimeRuntime != nullptr) {
    runtime.realtimeRuntime->gooseTimeoutWorker.request_stop();
    runtime.realtimeRuntime->realtimeConsumerWorker.request_stop();
  }
  if (!status.ok()) {
    runtime.state = IEC61850Proto::IED_STATE_ERROR;
    runtime.lastError = status.error_message();
    return status;
  }
  runtime.state = IEC61850Proto::IED_STATE_STOPPED;
  runtime.lastError.clear();
  LOG_INFO("IEC61850已停止IED通信功能: IED={}", connName);
  return grpc::Status::OK;
}

grpc::Status Manager::UpsertPointMappings(
    const IEC61850Proto::UpsertPointMappingsRequest& request) {
  auto status = ValidateName(request.conn_name(), "conn_name");
  if (!status.ok()) {
    return status;
  }
  std::lock_guard operationLock(operationMutex_);
  {
    std::lock_guard lock(mutex_);
    const auto* persisted = FindIed(config_, request.conn_name());
    if (persisted == nullptr) {
      return NotFound("IED连接不存在");
    }
    const auto runtimeIt = runtimeByConnName_.find(request.conn_name());
    if (runtimeIt != runtimeByConnName_.end() &&
        (runtimeIt->second.protocolSessionActive ||
         runtimeIt->second.mmsPipelineActive ||
         (runtimeIt->second.state != IEC61850Proto::IED_STATE_STOPPED &&
          runtimeIt->second.state != IEC61850Proto::IED_STATE_ERROR))) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IED通信资源尚未停止，不能修改点映射");
    }

    IEC61850Proto::PersistedConfig candidate = config_;
    auto* mappings = FindMappings(&candidate, request.conn_name());
    if (mappings == nullptr) {
      mappings = candidate.add_point_mappings();
      mappings->set_conn_name(request.conn_name());
    }
    if (request.replace()) {
      mappings->clear_points();
    }
    for (const auto& requestedPoint : request.points()) {
      IEC61850Proto::PointMapping* existing = nullptr;
      for (auto& point : *mappings->mutable_points()) {
        if (point.tag() == requestedPoint.tag() ||
            (point.data_ref() == requestedPoint.data_ref() &&
             point.fc() == requestedPoint.fc())) {
          existing = &point;
          break;
        }
      }
      if (existing != nullptr) {
        *existing = requestedPoint;
      } else {
        *mappings->add_points() = requestedPoint;
      }
    }
    std::vector<IEC61850Proto::ValidationIssue> issues;
    status = ValidatePersistedConfig(candidate, &issues);
    if (!status.ok()) {
      return status;
    }
    status = SaveCandidateLocked(candidate);
    if (!status.ok()) {
      return status;
    }
  }

  for (const auto& issue : ReconcileDataCenterOperation()) {
    LOG_WARNING("IEC61850点映射已保存但DataCenter对账降级: IED={}, 编码={}, 原因={}",
                request.conn_name(), issue.code(), issue.message());
  }
  return grpc::Status::OK;
}

grpc::Status Manager::GetPointMappings(
    const std::string& connName,
    IEC61850Proto::PointMappings* response) const {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response不能为空");
  }
  auto status = ValidateName(connName, "conn_name");
  if (!status.ok()) {
    return status;
  }
  std::lock_guard lock(mutex_);
  if (FindIed(config_, connName) == nullptr) {
    return NotFound("IED连接不存在");
  }
  response->Clear();
  response->set_conn_name(connName);
  if (const auto* mappings = FindMappings(config_, connName)) {
    *response = *mappings;
  }
  return grpc::Status::OK;
}

std::vector<IEC61850Proto::ValidationIssue>
Manager::ReconcileDataCenterOperation() {
  struct ReconcileTarget {
    std::string connName;
    std::vector<std::string> tags;
  };
  struct ReconcileResult {
    std::string connName;
    uint32_t connId = 0;
    bool available = false;
    std::string error;
  };

  std::vector<IEC61850Proto::ValidationIssue> issues;
  const auto addWarning = [&issues](std::string code, std::string path,
                                    std::string message) {
    auto& issue = issues.emplace_back();
    issue.set_severity(IEC61850Proto::VALIDATION_SEVERITY_WARNING);
    issue.set_code(std::move(code));
    issue.set_path(std::move(path));
    issue.set_message(std::move(message));
  };

  std::vector<std::string> pendingDeletes;
  {
    std::lock_guard lock(mutex_);
    std::unordered_set<std::string> uniqueDeletes;
    for (const auto& connName : config_.pending_connection_deletes()) {
      if (uniqueDeletes.emplace(connName).second) {
        pendingDeletes.emplace_back(connName);
      }
    }
    for (const auto& ied : config_.ieds()) {
      if (ied.pending_delete() &&
          uniqueDeletes.emplace(ied.config().conn_name()).second) {
        pendingDeletes.emplace_back(ied.config().conn_name());
      }
    }
  }
  for (const auto& connName : pendingDeletes) {
    const auto status = FinalizePendingDeleteOperation(connName);
    if (!status.ok()) {
      addWarning("DATACENTER_PENDING_DELETE_FAILED",
                 std::format("/pending_connection_deletes/{}", connName),
                 std::format("待删除DataCenter连接清理失败: IED={}, 原因={}",
                             connName, status.error_message()));
    }
  }

  std::vector<ReconcileTarget> targets;
  {
    std::lock_guard lock(mutex_);
    targets.reserve(config_.ieds_size());
    for (const auto& ied : config_.ieds()) {
      if (ied.pending_delete()) {
        continue;
      }
      ReconcileTarget target;
      target.connName = ied.config().conn_name();
      if (const auto* mappings = FindMappings(config_, target.connName)) {
        target.tags.reserve(mappings->points_size());
        for (const auto& point : mappings->points()) {
          target.tags.emplace_back(point.tag());
        }
      }
      targets.emplace_back(std::move(target));
    }
  }

  std::vector<ReconcileResult> results;
  results.reserve(targets.size());

  for (const auto& target : targets) {
    ReconcileResult result;
    result.connName = target.connName;
    DataCenterProto::ConnectionInfo connection;
    const auto connectionStatus = dataCenter_.GetOrCreateConnection(
        target.connName, &connection);
    if (!connectionStatus.ok()) {
      result.error = "DataCenter连接注册失败: " +
                     connectionStatus.error_message();
      addWarning("DATACENTER_CONNECTION_UNAVAILABLE",
                 std::format("/ieds/{}", target.connName), result.error);
      results.emplace_back(std::move(result));
      continue;
    }

    result.connId = connection.conn_id();
    const auto tagsStatus = dataCenter_.UpsertConnTags(
        result.connId, target.tags, true);
    if (!tagsStatus.ok()) {
      result.error = "DataCenter标签同步失败: " + tagsStatus.error_message();
      addWarning("DATACENTER_TAG_SYNC_FAILED",
                 std::format("/ieds/{}/points", target.connName),
                 result.error);
    } else {
      result.available = true;
    }
    results.emplace_back(std::move(result));
  }

  std::lock_guard lock(mutex_);
  IEC61850Proto::PersistedConfig candidate = config_;
  bool connIdsChanged = false;
  for (const auto& result : results) {
    if (result.connId != 0) {
      mmsPipeline_.UpdateConnectionId(result.connName, result.connId);
    }
    if (auto* persisted = FindIed(&candidate, result.connName);
        persisted != nullptr && result.connId != 0 &&
        persisted->conn_id() != result.connId) {
      persisted->set_conn_id(result.connId);
      connIdsChanged = true;
    }
    auto& runtime = runtimeByConnName_[result.connName];
    runtime.dataCenterAvailable = result.available;
    runtime.dataCenterError = result.error;
  }
  if (connIdsChanged) {
    const auto saveStatus = SaveCandidateLocked(candidate);
    if (!saveStatus.ok()) {
      addWarning("DATACENTER_CONN_ID_PERSIST_FAILED", "/ieds",
                 "DataCenter连接标识保存失败: " +
                     saveStatus.error_message());
    }
  }
  return issues;
}

grpc::Status Manager::GetRuntimeStatistics(
    const std::string& connName,
    IEC61850Proto::RuntimeStatistics* response) const {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response不能为空");
  }
  auto status = ValidateName(connName, "conn_name");
  if (!status.ok()) {
    return status;
  }
  {
    std::lock_guard lock(mutex_);
    const auto it = runtimeByConnName_.find(connName);
    if (it == runtimeByConnName_.end()) {
      return NotFound("IED连接不存在");
    }
    *response = it->second.statistics;
  }
  const auto mmsStatistics = mmsPipeline_.GetStatistics(connName);
  response->set_mms_reports_received(
      response->mms_reports_received() +
      mmsStatistics.mms_reports_received());
  response->set_mms_events_dropped(
      response->mms_events_dropped() + mmsStatistics.mms_events_dropped());
  response->set_mms_queue_high_watermark(std::max(
      response->mms_queue_high_watermark(),
      mmsStatistics.mms_queue_high_watermark()));
  response->set_data_center_batches_published(
      response->data_center_batches_published() +
      mmsStatistics.data_center_batches_published());
  response->set_data_center_publish_failures(
      response->data_center_publish_failures() +
      mmsStatistics.data_center_publish_failures());
  response->set_mms_values_unmapped(
      response->mms_values_unmapped() +
      mmsStatistics.mms_values_unmapped());
  response->set_mms_values_type_mismatch(
      response->mms_values_type_mismatch() +
      mmsStatistics.mms_values_type_mismatch());
  response->set_mms_values_invalid(
      response->mms_values_invalid() + mmsStatistics.mms_values_invalid());
  response->set_mms_values_deadband_filtered(
      response->mms_values_deadband_filtered() +
      mmsStatistics.mms_values_deadband_filtered());
  response->set_mms_values_oversized(
      response->mms_values_oversized() + mmsStatistics.mms_values_oversized());
  response->set_mms_reports_oversized(
      response->mms_reports_oversized() +
      mmsStatistics.mms_reports_oversized());
  response->set_mms_queue_bytes_high_watermark(std::max(
      response->mms_queue_bytes_high_watermark(),
      mmsStatistics.mms_queue_bytes_high_watermark()));
  {
    std::lock_guard lock(mutex_);
    const auto runtimeIt = runtimeByConnName_.find(connName);
    if (runtimeIt != runtimeByConnName_.end() &&
        runtimeIt->second.realtimeRuntime != nullptr) {
      const auto& realtime = runtimeIt->second.realtimeRuntime;
      response->set_goose_frames_received(
          response->goose_frames_received() +
          realtime->gooseFramesReceived.load(std::memory_order_acquire));
      response->set_goose_frames_invalid(
          response->goose_frames_invalid() +
          realtime->gooseFramesInvalid.load(std::memory_order_acquire));
      response->set_goose_timeouts(
          response->goose_timeouts() +
          realtime->gooseTimeouts.load(std::memory_order_acquire));
      response->set_sv_frames_received(
          response->sv_frames_received() +
          realtime->svFramesReceived.load(std::memory_order_acquire));
      response->set_sv_frames_invalid(
          response->sv_frames_invalid() +
          realtime->svFramesInvalid.load(std::memory_order_acquire));
      response->set_sv_samples_dropped(
          response->sv_samples_dropped() +
          realtime->svSamplesDropped.load(std::memory_order_acquire));
    }
  }
  response->set_last_event_ts_ms(std::max(response->last_event_ts_ms(),
                                          mmsStatistics.last_event_ts_ms()));
  response->set_conn_name(connName);
  return grpc::Status::OK;
}

grpc::Status Manager::ValidateName(std::string_view value,
                                   std::string_view fieldName) {
  if (value.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        std::format("{}不能为空", fieldName));
  }
  return grpc::Status::OK;
}

const IEC61850Proto::NormalizedSclModel* Manager::FindModel(
    const IEC61850Proto::PersistedConfig& config,
    std::string_view modelName) {
  for (const auto& model : config.models()) {
    if (model.model_name() == modelName) {
      return &model;
    }
  }
  return nullptr;
}

IEC61850Proto::PersistedIed* Manager::FindIed(
    IEC61850Proto::PersistedConfig* config, std::string_view connName) {
  for (auto& ied : *config->mutable_ieds()) {
    if (ied.config().conn_name() == connName) {
      return &ied;
    }
  }
  return nullptr;
}

const IEC61850Proto::PersistedIed* Manager::FindIed(
    const IEC61850Proto::PersistedConfig& config,
    std::string_view connName) {
  for (const auto& ied : config.ieds()) {
    if (ied.config().conn_name() == connName) {
      return &ied;
    }
  }
  return nullptr;
}

IEC61850Proto::PointMappings* Manager::FindMappings(
    IEC61850Proto::PersistedConfig* config, std::string_view connName) {
  for (auto& mappings : *config->mutable_point_mappings()) {
    if (mappings.conn_name() == connName) {
      return &mappings;
    }
  }
  return nullptr;
}

const IEC61850Proto::PointMappings* Manager::FindMappings(
    const IEC61850Proto::PersistedConfig& config,
    std::string_view connName) {
  for (const auto& mappings : config.point_mappings()) {
    if (mappings.conn_name() == connName) {
      return &mappings;
    }
  }
  return nullptr;
}

grpc::Status Manager::FillIedInfoLocked(
    const IEC61850Proto::PersistedIed& persisted,
    IEC61850Proto::IedInfo* response) const {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response不能为空");
  }
  response->Clear();
  *response->mutable_config() = persisted.config();
  response->set_conn_id(persisted.conn_id());
  const auto runtimeIt = runtimeByConnName_.find(persisted.config().conn_name());
  RuntimeState fallback;
  const auto& runtime = runtimeIt == runtimeByConnName_.end()
                            ? fallback
                            : runtimeIt->second;
  response->set_state(persisted.pending_delete()
                          ? IEC61850Proto::IED_STATE_PENDING_DELETE
                          : runtime.state);
  response->set_active_channel(runtime.activeChannel);
  if (runtime.lastError.empty()) {
    response->set_last_error(runtime.dataCenterError);
  } else if (runtime.dataCenterError.empty()) {
    response->set_last_error(runtime.lastError);
  } else {
    response->set_last_error(runtime.lastError + "; " +
                             runtime.dataCenterError);
  }
  response->set_data_center_available(runtime.dataCenterAvailable);
  if (runtime.state == IEC61850Proto::IED_STATE_STOPPED &&
      runtime.channels.empty() &&
      runtime.activeChannel == IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED) {
    return grpc::Status::OK;
  }
  for (const auto& channel : persisted.config().channels()) {
    auto* info = response->add_channels();
    *info->mutable_config() = channel;
    if (!channel.enabled()) {
      info->set_state(IEC61850Proto::CHANNEL_STATE_DISABLED);
    } else {
      const auto channelIt =
          runtime.channels.find(static_cast<int>(channel.channel()));
      if (channelIt != runtime.channels.end()) {
        info->set_state(channelIt->second.state);
        info->set_last_error(channelIt->second.lastError);
      } else if (runtime.activeChannel == channel.channel() &&
                 (runtime.state == IEC61850Proto::IED_STATE_RUNNING ||
                  runtime.state == IEC61850Proto::IED_STATE_DEGRADED)) {
        info->set_state(IEC61850Proto::CHANNEL_STATE_CONNECTED);
      } else {
        info->set_state(IEC61850Proto::CHANNEL_STATE_DISCONNECTED);
      }
    }
  }
  return grpc::Status::OK;
}

grpc::Status Manager::SaveCandidateLocked(
    const IEC61850Proto::PersistedConfig& candidate) {
  auto status = store_.Save(candidate);
  if (!status.ok()) {
    LOG_ERROR("IEC61850聚合配置保存失败: {}", status.error_message());
    return status;
  }
  config_ = candidate;
  for (const auto& ied : config_.ieds()) {
    auto [it, inserted] = runtimeByConnName_.try_emplace(
        ied.config().conn_name());
    if (inserted) {
      it->second.statistics.set_conn_name(ied.config().conn_name());
    }
  }
  return grpc::Status::OK;
}

void Manager::RebuildRuntimeLocked() {
  auto previous = std::move(runtimeByConnName_);
  runtimeByConnName_.clear();
  for (const auto& ied : config_.ieds()) {
    auto previousIt = previous.find(ied.config().conn_name());
    auto& runtime = runtimeByConnName_[ied.config().conn_name()];
    if (previousIt != previous.end()) {
      runtime = std::move(previousIt->second);
    }
    runtime.state = ied.pending_delete()
                        ? IEC61850Proto::IED_STATE_PENDING_DELETE
                        : IEC61850Proto::IED_STATE_STOPPED;
    runtime.activeChannel = IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
    AdvanceSessionGeneration(&runtime.sessionGeneration);
    runtime.protocolSessionActive = false;
    runtime.mmsPipelineActive = false;
    runtime.channels.clear();
    runtime.statistics.set_conn_name(ied.config().conn_name());
  }
}

}  // namespace IEC61850
