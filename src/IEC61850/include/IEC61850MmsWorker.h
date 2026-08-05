#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <stop_token>
#include <span>
#include <thread>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850MmsService.h"
#include "IEC61850MmsFile.h"
#include "IEC61850MmsControl.h"
#include "IEC61850MmsReport.h"
#include "IEC61850MmsSession.h"
#include "IEC61850MmsSettingGroup.h"
#include "IEC61850MmsTransport.h"
#include "IEC61850ProtocolStack.h"

namespace IEC61850 {

// 自研MMS会话工作器；负责ISO/MMS建链、在线目录和RCB参数核对，
// 并在唯一配置通道上执行分阶段RCB Write及InformationReport接收。
class MmsSessionWorker {
public:
  using MmsTransportFactory = std::function<std::unique_ptr<MmsTransport>(
      const MmsTransportEndpoint&, IEC61850Proto::NetworkChannel)>;

  MmsSessionWorker(const ProtocolIedPlan& plan,
                   std::vector<ProtocolNetworkBinding> bindings,
                   ProtocolEventCallbacks callbacks,
                   MmsTransportFactory transportFactory = {});
  ~MmsSessionWorker();

  MmsSessionWorker(const MmsSessionWorker&) = delete;
  MmsSessionWorker& operator=(const MmsSessionWorker&) = delete;

  grpc::Status Start();
  void Stop() noexcept;

  // 向当前已就绪的活动MMS通道提交一个串行Read请求。
  grpc::Status ReadMms(const MmsReadRequest& request,
                       MmsReadResponse* response);
  // 向当前已就绪的活动MMS通道提交一个串行Write请求。
  grpc::Status WriteMms(const MmsWriteRequest& request,
                        MmsWriteResponse* response);

  // 获取远端文件目录；会在当前活动MMS通道上自动处理FileDirectory分页。
  grpc::Status ListFiles(const MmsFileDirectoryRequest& request,
                         std::vector<MmsFileDirectoryEntry>* entries,
                         std::size_t maxEntries = 4096,
                         std::optional<std::chrono::milliseconds> timeout =
                             std::nullopt,
                         std::shared_ptr<std::atomic_bool> cancellation = {});
  // 下载远端文件；内部串行执行FileOpen/FileRead/FileClose并原子落盘。
  grpc::Status DownloadFile(const MmsFileDownloadRequest& request,
                            MmsFileDownloadResult* result);

  // 普通SBO选择；请求仍通过当前活动通道的串行Read队列执行。
  grpc::Status SelectMmsControl(const MmsObjectName& controlObject,
                                MmsReadResponse* response,
                                std::optional<std::chrono::milliseconds>
                                    timeout = std::nullopt,
                                std::shared_ptr<std::atomic_bool>
                                    cancellation = {});
  // 执行SBOw、Oper或Cancel；请求仍通过当前活动通道的串行Write队列执行。
  grpc::Status WriteMmsControl(const MmsControlCommand& command,
                               MmsWriteResponse* response);

  // 按在线ctlModel自动完成直控或SBO/SBOw选择后Oper。
  grpc::Status ExecuteMmsPointControl(
      const MmsPointControlCommand& command,
      MmsWriteResponse* response);

  // SGCB/定值组高层流程；底层仍复用当前活动通道的串行Read/Write队列。
  grpc::Status ReadSettingGroupStatus(const MmsSettingGroupPlan& plan,
                                      MmsSettingGroupStatus* status);
  grpc::Status SelectSettingGroup(const MmsSettingGroupPlan& plan,
                                  std::uint32_t group);
  grpc::Status ConfirmSettingGroupEdit(const MmsSettingGroupPlan& plan);
  grpc::Status CancelSettingGroupEdit(const MmsSettingGroupPlan& plan);
  grpc::Status ActivateSettingGroup(const MmsSettingGroupPlan& plan,
                                    std::uint32_t group);

private:
  struct Channel;
  struct SharedState;
  struct ControlRequest;
  struct ControlExchangeResult {
    bool requestSent = false;
    bool commandTerminationReceived = false;
    bool commandTerminationSucceeded = false;
    bool commandTerminationRejected = false;
    bool commandTerminationMalformed = false;
    bool commandTerminationTransportFailed = false;
    bool cancelCompleted = false;
  };

  void Run(std::size_t index, std::stop_token stopToken);
  void ProcessControlRequests(Channel& channel, std::stop_token stopToken);
  std::shared_ptr<ControlRequest> TakeControlRequest(Channel& channel);
  std::shared_ptr<ControlRequest> TakePendingCancelRequest(
      Channel& channel, const MmsObjectName& controlObject);
  void CompleteControlRequest(const std::shared_ptr<ControlRequest>& request,
                              const grpc::Status& status);
  grpc::Status WriteMmsWithTimeout(
      const MmsWriteRequest& request, MmsWriteResponse* response,
      std::chrono::milliseconds timeout);
  grpc::Status ReadMmsOnChannel(Channel& channel,
                                const MmsReadRequest& request,
                                MmsReadResponse* response,
                                std::chrono::milliseconds timeout,
                                std::shared_ptr<std::atomic_bool>
                                    cancellation = {});
  grpc::Status WriteMmsOnChannelWithTimeout(
      Channel& channel, const MmsWriteRequest& request,
      MmsWriteResponse* response, std::chrono::milliseconds timeout,
      bool awaitCommandTermination = false,
      const MmsObjectName* expectedOper = nullptr,
      std::uint8_t expectedControlNumber = 0,
      ControlExchangeResult* exchangeResult = nullptr,
      std::optional<MmsControlOperation> controlOperation = std::nullopt,
      const MmsObjectName* controlObject = nullptr,
      std::shared_ptr<std::atomic_bool> cancellation = {});
  grpc::Status WaitForCommandTermination(
      Channel& channel, const std::shared_ptr<ControlRequest>& request,
      std::stop_token stopToken, std::chrono::milliseconds timeout);
  grpc::Status SubmitControlRequest(
      Channel& channel, const std::shared_ptr<ControlRequest>& request,
      std::chrono::milliseconds timeout);
  void CancelControlRequests(Channel& channel, grpc::Status status);
  grpc::Status ValidateControlForActiveChannel(
      const MmsObjectName& controlObject, MmsControlOperation operation,
      Channel** target,
      std::shared_ptr<const MmsControlModel>* model);
  grpc::Status GetActiveControlCapability(
      const MmsObjectName& controlObject,
      MmsControlCapability* capability);
  std::uint8_t AllocateControlNumber();
  bool IsActiveChannel(IEC61850Proto::NetworkChannel channel) const;
  grpc::Status Establish(Channel& channel, std::stop_token stopToken);
  grpc::Status ReadNameList(Channel& channel, MmsObjectClass objectClass,
                            MmsObjectScope scope,
                            std::stop_token stopToken,
                            std::uint32_t* nextInvokeId,
                            std::vector<std::string>* identifiers,
                            const std::function<void(
                                std::span<const std::uint8_t>)>&
                                onUnconfirmed);
  grpc::Status ReadOnlineDirectory(Channel& channel,
                                   std::stop_token stopToken,
                                   bool supportsRead,
                                   bool supportsVariableAttributes,
                                   bool supportsNamedVariableListAttributes,
                                   bool supportsWrite);
  grpc::Status ReadIdentify(Channel& channel, std::stop_token stopToken,
                            std::uint32_t* nextInvokeId);
  grpc::Status ConfigureReportControls(Channel& channel,
                                       std::stop_token stopToken,
                                       std::uint32_t* nextInvokeId,
                                       MmsSessionContract* contract);
  bool TryClaimRcbConfiguration(IEC61850Proto::NetworkChannel channel);
  void ReleaseRcbConfiguration(IEC61850Proto::NetworkChannel channel);
  void EnqueueUnconfirmed(IEC61850Proto::NetworkChannel channel,
                          std::span<const std::uint8_t> pdu);
  bool ConsumeRcbReconfigurationRequest(
      IEC61850Proto::NetworkChannel channel);
  bool SetChannelReady(IEC61850Proto::NetworkChannel channel, bool ready);
  void ProcessMmsInformationReport(
      Channel& channel, std::span<const std::uint8_t> pdu,
      const std::vector<MmsReportDecodePlan>& reportPlans);
  void DrainPendingUnconfirmed(
      Channel& channel, const std::vector<MmsReportDecodePlan>& reportPlans);
  void PublishState(MmsConnectionEventType type, ProtocolSessionState state,
                    std::size_t reconnectIndex, std::string error);
  void UpdateChannel(std::size_t index, IEC61850Proto::ChannelState state,
                     std::string error);

  std::vector<std::unique_ptr<Channel>> channels_;
  ProtocolIedPlan plan_;
  ProtocolEventCallbacks callbacks_;
  MmsTransportFactory transportFactory_;
  std::shared_ptr<SharedState> sharedState_;
  std::vector<std::jthread> workers_;
  std::mutex lifecycleMutex_;
  std::mutex controlNumberMutex_;
  std::uint16_t nextControlNumber_ = 1;
  bool started_ = false;
};

}  // namespace IEC61850
