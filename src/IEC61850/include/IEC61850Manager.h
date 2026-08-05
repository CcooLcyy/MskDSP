#pragma once

#include <cstdint>
#include <atomic>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <grpcpp/support/status.h>

#include "DataCenter.grpc.pb.h"
#include "IEC61850.pb.h"
#include "IEC61850ConfigStore.h"
#include "IEC61850DataCenterClient.h"
#include "IEC61850MmsPipeline.hpp"
#include "IEC61850ProtectionActionDispatcher.h"
#include "IEC61850ProtectionEngine.h"
#include "IEC61850ProtocolStack.h"
#include "IEC61850RealtimeSignalBus.h"
#include "IEC61850RealtimeSignalProcessor.h"
#include "IEC61850SvMathEngine.h"
#include "IEC61850GooseRuntime.h"
#include "IEC61850SvRuntime.h"
#include "IEC61850SclParser.h"
#include "IEC61850SvState.h"

namespace IEC61850 {

class Manager {
public:
  explicit Manager(
      std::filesystem::path databasePath = std::filesystem::path("./conf/config.db"),
      std::shared_ptr<ProtocolStackAdapter> protocolStack = {});
  ~Manager();

  void SetDataCenterStub(
      std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);
  void SetProtocolStack(std::shared_ptr<ProtocolStackAdapter> protocolStack);

  grpc::Status LoadPersistedConfig();
  void RestoreConfiguredIeds();
  void ReconcileDataCenter();
  void Shutdown();

  grpc::Status ApplyTargetConfig(
      const IEC61850Proto::ApplyTargetConfigRequest& request,
      IEC61850Proto::ApplyTargetConfigResponse* response);

  grpc::Status ImportScl(const IEC61850Proto::ImportSclRequest& request,
                         IEC61850Proto::ImportSclResponse* response);
  grpc::Status GetModelSummary(
      const std::string& modelName,
      IEC61850Proto::SclModelSummary* response) const;
  grpc::Status ListModels(IEC61850Proto::ListModelsResponse* response) const;
  grpc::Status DeleteModel(const std::string& modelName);

  grpc::Status UpsertIed(const IEC61850Proto::UpsertIedRequest& request,
                         IEC61850Proto::IedInfo* response);
  grpc::Status GetIed(const std::string& connName,
                      IEC61850Proto::IedInfo* response) const;
  grpc::Status ListIeds(IEC61850Proto::ListIedsResponse* response) const;
  grpc::Status DeleteIed(const std::string& connName);
  grpc::Status StartIed(const std::string& connName);
  grpc::Status StopIed(const std::string& connName);

  // 供下位机控制策略调用的受控MMS变量读取入口。
  grpc::Status ReadMms(const std::string& connName,
                       const MmsReadRequest& request,
                       MmsReadResponse* response);
  // 供下位机控制策略调用的受控MMS变量写入入口。
  grpc::Status WriteMms(const std::string& connName,
                        const MmsWriteRequest& request,
                        MmsWriteResponse* response);
  // 供下位机控制策略调用的普通SBO选择入口，不暴露原始MMS gRPC。
  grpc::Status SelectMmsControl(const std::string& connName,
                                const MmsObjectName& controlObject,
                                MmsReadResponse* response);
  // 供下位机控制策略调用的SBOw/Oper/Cancel入口，不暴露原始MMS gRPC。
  grpc::Status WriteMmsControl(const std::string& connName,
                               const MmsControlCommand& command,
                               MmsWriteResponse* response);

  grpc::Status ReadSettingGroupStatus(
      const std::string& connName, const MmsSettingGroupPlan& plan,
      MmsSettingGroupStatus* status);
  grpc::Status SelectSettingGroup(const std::string& connName,
                                  const MmsSettingGroupPlan& plan,
                                  std::uint32_t group);
  grpc::Status ConfirmSettingGroupEdit(const std::string& connName,
                                       const MmsSettingGroupPlan& plan);
  grpc::Status CancelSettingGroupEdit(const std::string& connName,
                                      const MmsSettingGroupPlan& plan);
  grpc::Status ActivateSettingGroup(const std::string& connName,
                                    const MmsSettingGroupPlan& plan,
                                    std::uint32_t group);

  // DataCenter CommandExecutor使用的同步MMS控制入口。
  grpc::Status ExecuteDataCenterCommand(
      const DataCenterProto::ExecuteCommandRequest& request,
      DataCenterProto::ExecuteCommandResponse* response,
      std::shared_ptr<std::atomic_bool> cancellation = {});

  // 供下位机保护/联锁引擎调用的内部GOOSE发布入口。
  grpc::Status PublishGoose(const std::string& connName,
                            std::uint32_t subscriptionId,
                            std::span<const ProtocolRealtimeValue> values,
                            bool stateChanged);

  grpc::Status UpsertPointMappings(
      const IEC61850Proto::UpsertPointMappingsRequest& request);
  grpc::Status GetPointMappings(
      const std::string& connName,
      IEC61850Proto::PointMappings* response) const;
  grpc::Status GetRuntimeStatistics(
      const std::string& connName,
      IEC61850Proto::RuntimeStatistics* response) const;

private:
  struct CallbackGate {
    std::mutex mutex;
    Manager* owner = nullptr;
  };

  struct RuntimeChannelState {
    IEC61850Proto::ChannelState state =
        IEC61850Proto::CHANNEL_STATE_UNSPECIFIED;
    std::string lastError;
  };

  struct RealtimeRuntime {
    struct GooseRoute {
      std::uint32_t subscriptionId = 0;
      std::vector<std::uint32_t> signalIds;
      std::vector<ProtocolRealtimeValueType> valueTypes;
      std::array<std::size_t, 3> producerIndices{
          std::numeric_limits<std::size_t>::max(),
          std::numeric_limits<std::size_t>::max(),
          std::numeric_limits<std::size_t>::max()};
    };

    struct SvRoute {
      std::uint32_t streamId = 0;
      std::vector<std::uint32_t> signalIds;
      std::vector<ProtocolRealtimeValueType> valueTypes;
      std::array<std::size_t, 3> producerIndices{
          std::numeric_limits<std::size_t>::max(),
          std::numeric_limits<std::size_t>::max(),
          std::numeric_limits<std::size_t>::max()};
    };

    std::uint64_t sessionGeneration = 0;
    std::shared_ptr<RealtimeSignalBus> bus;
    std::shared_ptr<RealtimeSignalProcessor> signalProcessor;
    std::shared_ptr<ProtectionEngine> protectionEngine;
    std::shared_ptr<ProtectionActionDispatcher> protectionActionDispatcher;
    std::shared_ptr<ProtocolStackAdapter> protocolStack;
    std::string connName;
    std::vector<RealtimeSignalBus::Producer> producers;
    std::vector<GooseRoute> gooseRoutes;
    std::vector<SvRoute> svRoutes;
    std::shared_ptr<GooseRealtimeEngine> gooseEngine;
    std::shared_ptr<SvRealtimeEngine> svEngine;
    std::shared_ptr<SvMathEngine> svMathEngine;
    ThreadRuntimePolicy runtimePolicy;
    ThreadRuntimeState gooseTimeoutRuntimeState;
    ThreadRuntimeState realtimeConsumerRuntimeState;
    std::jthread gooseTimeoutWorker;
    std::jthread realtimeConsumerWorker;
    std::atomic<std::uint64_t> gooseFramesReceived = 0;
    std::atomic<std::uint64_t> gooseFramesInvalid = 0;
    std::atomic<std::uint64_t> gooseTimeouts = 0;
    std::atomic<std::uint64_t> svFramesReceived = 0;
    std::atomic<std::uint64_t> svFramesInvalid = 0;
    std::atomic<std::uint64_t> svSamplesDropped = 0;
    std::atomic<std::uint64_t> realtimeUpdatesConsumed = 0;
  };

  struct RuntimeState {
    IEC61850Proto::IedState state = IEC61850Proto::IED_STATE_STOPPED;
    IEC61850Proto::NetworkChannel activeChannel =
        IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
    std::string lastError;
    std::string dataCenterError;
    bool dataCenterAvailable = false;
    uint64_t sessionGeneration = 0;
    bool protocolSessionActive = false;
    bool mmsPipelineActive = false;
    std::shared_ptr<RealtimeRuntime> realtimeRuntime;
    std::unordered_map<int, RuntimeChannelState> channels;
    IEC61850Proto::RuntimeStatistics statistics;
  };

  static grpc::Status ValidateName(std::string_view value,
                                   std::string_view fieldName);
  static const IEC61850Proto::NormalizedSclModel* FindModel(
      const IEC61850Proto::PersistedConfig& config,
      std::string_view modelName);
  static IEC61850Proto::PersistedIed* FindIed(
      IEC61850Proto::PersistedConfig* config, std::string_view connName);
  static const IEC61850Proto::PersistedIed* FindIed(
      const IEC61850Proto::PersistedConfig& config,
      std::string_view connName);
  static IEC61850Proto::PointMappings* FindMappings(
      IEC61850Proto::PersistedConfig* config, std::string_view connName);
  static const IEC61850Proto::PointMappings* FindMappings(
      const IEC61850Proto::PersistedConfig& config,
      std::string_view connName);
  grpc::Status FillIedInfoLocked(const IEC61850Proto::PersistedIed& persisted,
                                 IEC61850Proto::IedInfo* response) const;
  grpc::Status SaveCandidateLocked(
      const IEC61850Proto::PersistedConfig& candidate);
  void RebuildRuntimeLocked();
  grpc::Status StartIedOperation(const std::string& connName,
                                 bool persistDesiredState);
  grpc::Status StopIedOperation(const std::string& connName,
                                bool persistDesiredState);
  void HandleMmsConnectionEvent(std::string connName,
                                uint64_t sessionGeneration,
                                MmsConnectionEvent event);
  void HandleGooseFrame(std::string connName, uint64_t sessionGeneration,
                        ProtocolGooseFrameView frame);
  void HandleSvFrame(std::string connName, uint64_t sessionGeneration,
                     ProtocolSvFrameView frame);
  static void PublishGooseFrame(const std::shared_ptr<RealtimeRuntime>& realtime,
                                std::uint64_t sessionGeneration,
                                ProtocolGooseFrameView frame) noexcept;
  static void PublishSvFrame(const std::shared_ptr<RealtimeRuntime>& realtime,
                             std::uint64_t sessionGeneration,
                             ProtocolSvFrameView frame) noexcept;
  static void StopRealtimeWorkers(
      const std::shared_ptr<RealtimeRuntime>& realtime) noexcept;
  grpc::Status FinalizePendingDeleteOperation(const std::string& connName);
  std::vector<IEC61850Proto::ValidationIssue>
  ReconcileDataCenterOperation();

  // 生命周期/配置操作使用排他锁；同步控制使用共享锁，允许不同IED并行执行。
  std::shared_mutex operationMutex_;
  mutable std::mutex mutex_;
  IEC61850Proto::PersistedConfig config_;
  std::unordered_map<std::string, RuntimeState> runtimeByConnName_;
  ConfigStore store_;
  SclParser parser_;
  DataCenterClient dataCenter_;
  MmsEventPipeline mmsPipeline_;
  std::shared_ptr<ProtocolStackAdapter> protocolStack_;
  std::shared_ptr<CallbackGate> callbackGate_;
  bool shuttingDown_ = false;
};

}  // namespace IEC61850
