#include "IEC61850MmsWorker.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <exception>
#include <format>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <string_view>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "IEC61850MmsBer.h"
#include "IEC61850MmsChannelPolicy.h"
#include "IEC61850MmsExchange.h"
#include "IEC61850MmsIsoSession.h"
#include "IEC61850MmsPdu.h"
#include "IEC61850MmsReport.h"
#include "IEC61850MmsRcb.h"
#include "IEC61850MmsService.h"
#include "IEC61850MmsSession.h"
#include "Logger.h"

namespace IEC61850 {
namespace {

constexpr std::uint32_t kMmsIoTimeoutMs = 1000;
constexpr std::uint32_t kMmsRunReceivePollMs = 50;
constexpr std::uint32_t kMmsConnectTimeoutMs = 3000;
constexpr std::uint32_t kMmsRetryDelayMs = 1000;
constexpr std::uint32_t kMmsCommandTerminationPollMs = 20;
constexpr std::size_t kMmsPduBufferSize = 4096;
constexpr std::size_t kMmsNameListPageLimit = 4096;
constexpr std::size_t kMmsNameListEntryLimit = 65536;
constexpr std::size_t kPendingUnconfirmedPduLimit = 256;
constexpr std::size_t kPendingUnconfirmedBytesLimit = 4 * 1024 * 1024;
constexpr std::uint32_t kMmsControlTimeoutMs = 5000;
constexpr std::size_t kMmsControlQueueLimit = 64;
constexpr std::uint8_t kMmsSupportGetNameList = 0x40;
constexpr std::uint8_t kMmsSupportIdentify = 0x10;
constexpr std::uint8_t kMmsSupportRead = 0x08;
constexpr std::uint8_t kMmsSupportWrite = 0x04;
constexpr std::uint8_t kMmsSupportGetVariableAccessAttributes = 0x02;
constexpr std::uint8_t kMmsSupportGetNamedVariableListAttributes = 0x08;
// ConfirmedService编号72/73/74/77分别对应byte[9]中的高位到低位。
constexpr std::uint8_t kMmsSupportFileOpen = 0x80;
constexpr std::uint8_t kMmsSupportFileRead = 0x40;
constexpr std::uint8_t kMmsSupportFileClose = 0x20;
constexpr std::uint8_t kMmsSupportFileDirectory = 0x04;

std::uint32_t DefaultMmsAssociationBudgetMs(
    const MmsTransportEndpoint& endpoint) {
  // TCP连接/COTP确认由传输层消耗两份I/O窗口，Session CONNECT/ACCEPT再消耗
  // 两份I/O窗口；这里把四个阶段合并为一次关联建立预算。
  const auto total = static_cast<std::uint64_t>(endpoint.connectTimeoutMs) +
                     static_cast<std::uint64_t>(endpoint.ioTimeoutMs) * 4u;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(
      total, std::numeric_limits<std::uint32_t>::max()));
}

std::uint32_t RemainingTimeoutMs(
    std::chrono::steady_clock::time_point deadline) {
  const auto remaining = deadline - std::chrono::steady_clock::now();
  if (remaining <= std::chrono::steady_clock::duration::zero()) {
    return 0;
  }
  auto timeout =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  if (timeout <= std::chrono::milliseconds::zero()) {
    timeout = std::chrono::milliseconds(1);
  } else if (timeout < remaining) {
    ++timeout;
  }
  return static_cast<std::uint32_t>(std::min<std::int64_t>(
      timeout.count(), static_cast<std::int64_t>(
                           std::numeric_limits<std::uint32_t>::max())));
}

std::string HexDump(std::span<const std::uint8_t> bytes) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 3);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index != 0) {
      result.push_back(' ');
    }
    result.push_back(kHex[bytes[index] >> 4]);
    result.push_back(kHex[bytes[index] & 0x0f]);
  }
  return result;
}

bool IsCancellationRequested(
    const std::shared_ptr<std::atomic_bool>& cancellation) noexcept {
  return cancellation != nullptr &&
         cancellation->load(std::memory_order_acquire);
}

bool SameObjectName(const MmsObjectName& left,
                   const MmsObjectName& right) noexcept {
  return left.type == right.type && left.domain == right.domain &&
         left.identifier == right.identifier;
}

std::int64_t NowMs() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool IsSameOid(const MmsAareView& aare,
               std::span<const std::uint32_t> expected) noexcept {
  return aare.applicationContextOidSize == expected.size() &&
         std::equal(aare.applicationContextOid.begin(),
                    aare.applicationContextOid.begin() +
                        aare.applicationContextOidSize,
                    expected.begin());
}

MmsInitiateRequest DefaultInitiateRequest(bool includeWrite) {
  MmsInitiateRequest request;
  request.proposedParameterSupport.size = 2;
  request.proposedParameterSupport.unusedBits = 5;
  request.proposedServiceSupport.size = 11;
  request.proposedServiceSupport.unusedBits = 3;
  // 服务支持位按MMS ConfirmedService顺序编码；includeWrite决定是否提出Write。
  request.proposedServiceSupport.bytes[0] =
      kMmsSupportGetNameList | kMmsSupportIdentify | kMmsSupportRead |
      kMmsSupportGetVariableAccessAttributes;
  if (includeWrite) {
    request.proposedServiceSupport.bytes[0] = static_cast<std::uint8_t>(
        request.proposedServiceSupport.bytes[0] | kMmsSupportWrite);
  }
  request.proposedServiceSupport.bytes[1] =
      kMmsSupportGetNamedVariableListAttributes;
  request.proposedServiceSupport.bytes[9] =
      kMmsSupportFileOpen | kMmsSupportFileRead | kMmsSupportFileClose |
      kMmsSupportFileDirectory;
  return request;
}

bool PlanRequiresMmsWrite(const ProtocolIedPlan& plan) noexcept {
  return plan.ied.report_controls_size() != 0 ||
         std::ranges::any_of(plan.ied.data_attributes(), [](const auto& item) {
           return item.fc() ==
                  IEC61850Proto::FUNCTIONAL_CONSTRAINT_CO;
         });
}

bool IsChannelConnected(const MmsChannelStatus& status) noexcept {
  return status.state == IEC61850Proto::CHANNEL_STATE_CONNECTED;
}

// 当前MMS请求按顺序发送并等待响应，未存在并发请求时可以从最大值安全回绕到1；
// invokeID为0保留给未初始化状态，不能出现在任何线上请求中。
grpc::Status AdvanceInvokeId(std::uint32_t* invokeId) {
  if (invokeId == nullptr || *invokeId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS invokeID未初始化");
  }
  *invokeId = *invokeId == std::numeric_limits<std::uint32_t>::max()
                  ? 1
                  : *invokeId + 1;
  return grpc::Status::OK;
}

bool IsBitStringSubset(const MmsBitString& value,
                       const MmsBitString& capability) noexcept {
  if (value.size != capability.size || value.unusedBits != capability.unusedBits) {
    return false;
  }
  for (std::size_t index = 0; index < value.size; ++index) {
    if ((value.bytes[index] &
         static_cast<std::uint8_t>(~capability.bytes[index])) != 0) {
      return false;
    }
  }
  return true;
}

bool IsQualityDataAttribute(const IEC61850Proto::SclIed& ied,
                            std::string_view dataRef,
                            IEC61850Proto::FunctionalConstraint fc) {
  const auto it = std::ranges::find_if(
      ied.data_attributes(), [&](const auto& attribute) {
        return attribute.data_ref() == dataRef && attribute.fc() == fc;
      });
  if (it != ied.data_attributes().end()) {
    std::string basicType = it->basic_type();
    std::ranges::transform(basicType, basicType.begin(), [](char value) {
      return static_cast<char>(
          std::toupper(static_cast<unsigned char>(value)));
    });
    if (basicType == "QUALITY") {
      return true;
    }
  }
  return dataRef.ends_with(".q") || dataRef.ends_with("$q");
}

bool NeedsOnlineControlValue(std::string_view dataRef) noexcept {
  return dataRef.ends_with(".ctlModel") ||
         dataRef.ends_with(".sboTimeout") ||
         dataRef.ends_with(".operTimeout");
}

std::vector<MmsReportDecodePlan> BuildMmsReportDecodePlans(
    const ProtocolIedPlan& plan) {
  std::vector<MmsReportDecodePlan> result;
  result.reserve(plan.ied.report_controls_size());
  for (const auto& control : plan.ied.report_controls()) {
    MmsReportDecodePlan decodePlan;
    decodePlan.reportRef = control.rcb_ref();
    decodePlan.reportId = control.report_id();
    decodePlan.dataSetRef = control.data_set_ref();
    decodePlan.confRev = control.config_revision();
    decodePlan.optionalFields = control.optional_fields();
    const auto dataSet = std::ranges::find_if(
        plan.ied.data_sets(), [&](const auto& candidate) {
          return candidate.data_set_ref() == control.data_set_ref();
        });
    if (dataSet != plan.ied.data_sets().end()) {
      decodePlan.members.reserve(dataSet->members_size());
      for (const auto& member : dataSet->members()) {
        decodePlan.members.push_back(
            {member.data_ref(), member.fc(),
             IsQualityDataAttribute(plan.ied, member.data_ref(), member.fc())});
      }
    }
    result.emplace_back(std::move(decodePlan));
  }
  return result;
}

}  // namespace

struct MmsSessionWorker::ControlRequest {
  enum class Kind { READ, WRITE, FILE_DIRECTORY, FILE_DOWNLOAD };

  Kind kind = Kind::READ;
  MmsReadRequest readRequest;
  MmsWriteRequest writeRequest;
  MmsReadResponse readResponse;
  MmsWriteResponse writeResponse;
  MmsFileDirectoryRequest fileDirectoryRequest;
  std::vector<MmsFileDirectoryEntry> fileDirectoryResponse;
  std::size_t fileDirectoryMaxEntries = 0;
  MmsFileDownloadRequest fileDownloadRequest;
  MmsFileDownloadResult fileDownloadResult;
  grpc::Status status;
  std::mutex mutex;
  std::condition_variable condition;
  bool awaitCommandTermination = false;
  std::optional<MmsControlOperation> controlOperation;
  MmsObjectName controlObject;
  MmsObjectName expectedOper;
  std::uint8_t expectedControlNumber = 0;
  std::chrono::milliseconds commandTimeout{0};
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
  std::shared_ptr<std::atomic_bool> cancellation;
  std::vector<std::vector<std::uint8_t>> unconfirmedDuringControl;
  bool done = false;
  bool cancelled = false;
  bool requestSent = false;
  bool commandTerminationReceived = false;
  bool commandTerminationSucceeded = false;
  bool commandTerminationRejected = false;
  bool commandTerminationMalformed = false;
  bool commandTerminationTransportFailed = false;
  bool cancelCompleted = false;
};

struct MmsSessionWorker::Channel {
  MmsTransportEndpoint endpoint;
  IEC61850Proto::NetworkChannel channel =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  std::unique_ptr<MmsTransport> transport;
  std::uint32_t nextInvokeId = 1;
  std::mutex controlMutex;
  std::deque<std::shared_ptr<ControlRequest>> controlRequests;
  bool acceptControlRequests = false;
  bool supportsWrite = false;
  bool supportsFileDirectory = false;
  bool supportsFileTransfer = false;
  std::shared_ptr<const MmsControlModel> controlModel;
  MmsSboState sboState;
  // 每个物理会话独立合并报告，避免A/B通道或重连代际交叉拼接分段。
  MmsReportAssembler reportAssembler;
  std::unique_ptr<MmsSessionContract> contract;
};

struct MmsSessionWorker::SharedState {
  struct PendingUnconfirmed {
    IEC61850Proto::NetworkChannel channel =
        IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
    std::vector<std::uint8_t> pdu;
  };

  std::mutex mutex;
  std::deque<PendingUnconfirmed> pendingUnconfirmed;
  std::size_t pendingUnconfirmedBytes = 0;
  std::unordered_set<int> rcbReconfigurationRequests;
  std::vector<MmsChannelStatus> channels;
  IEC61850Proto::NetworkChannel activeChannel =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  std::optional<IEC61850Proto::NetworkChannel> rcbConfigurationChannel;
  std::unordered_set<int> readyChannels;
};

MmsSessionWorker::MmsSessionWorker(
    const ProtocolIedPlan& plan, std::vector<ProtocolNetworkBinding> bindings,
    ProtocolEventCallbacks callbacks, MmsTransportFactory transportFactory)
    : plan_(plan),
      callbacks_(std::move(callbacks)),
      transportFactory_(std::move(transportFactory)),
      sharedState_(std::make_shared<SharedState>()) {
  sharedState_->channels.reserve(bindings.size());
  for (const auto& binding : bindings) {
    auto channel = std::make_unique<Channel>();
    channel->channel = binding.channel.channel();
    channel->endpoint.interfaceName = binding.channel.interface_name();
    channel->endpoint.localIp = binding.channel.local_ip();
    channel->endpoint.remoteIp = binding.channel.remote_ip();
    channel->endpoint.remotePort =
        static_cast<std::uint16_t>(binding.channel.remote_port());
    channel->endpoint.connectTimeoutMs = kMmsConnectTimeoutMs;
    channel->endpoint.ioTimeoutMs = kMmsIoTimeoutMs;
    channels_.emplace_back(std::move(channel));

    MmsChannelStatus status;
    status.channel = binding.channel.channel();
    status.state = IEC61850Proto::CHANNEL_STATE_DISCONNECTED;
    sharedState_->channels.emplace_back(std::move(status));
  }
}

MmsSessionWorker::~MmsSessionWorker() { Stop(); }

grpc::Status MmsSessionWorker::ReadMms(const MmsReadRequest& request,
                                       MmsReadResponse* response) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Read控制响应输出为空");
  }
  response->items.clear();
  if (request.variables.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Read控制请求不能为空");
  }
  {
    std::lock_guard lock(lifecycleMutex_);
    if (!started_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS会话工作器尚未启动");
    }
  }

  Channel* target = nullptr;
  {
    std::lock_guard lock(sharedState_->mutex);
    const auto active = sharedState_->activeChannel;
    if (active == IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED ||
        !sharedState_->readyChannels.contains(static_cast<int>(active))) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS当前没有已就绪的活动通道");
    }
    for (const auto& channel : channels_) {
      if (channel->channel == active) {
        target = channel.get();
        break;
      }
    }
  }
  if (target == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS活动通道不存在");
  }

  return ReadMmsOnChannel(*target, request, response,
                          std::chrono::milliseconds(kMmsControlTimeoutMs));
}

grpc::Status MmsSessionWorker::ReadMmsOnChannel(
    Channel& target, const MmsReadRequest& request, MmsReadResponse* response,
    std::chrono::milliseconds timeout,
    std::shared_ptr<std::atomic_bool> cancellation) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Read控制响应输出为空");
  }
  response->items.clear();
  if (request.variables.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Read控制请求不能为空");
  }

  auto command = std::make_shared<ControlRequest>();
  command->kind = ControlRequest::Kind::READ;
  command->readRequest = request;
  command->commandTimeout = timeout;
  command->deadline = std::chrono::steady_clock::now() + timeout;
  command->cancellation = std::move(cancellation);
  const auto status = SubmitControlRequest(target, command, timeout);
  if (status.ok()) {
    *response = std::move(command->readResponse);
  }
  return status;
}

grpc::Status MmsSessionWorker::WriteMms(const MmsWriteRequest& request,
                                        MmsWriteResponse* response) {
  return WriteMmsWithTimeout(request, response,
                             std::chrono::milliseconds(kMmsControlTimeoutMs));
}

grpc::Status MmsSessionWorker::ListFiles(
    const MmsFileDirectoryRequest& request,
    std::vector<MmsFileDirectoryEntry>* entries, std::size_t maxEntries,
    std::optional<std::chrono::milliseconds> timeout,
    std::shared_ptr<std::atomic_bool> cancellation) {
  if (entries == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS文件目录输出为空");
  }
  entries->clear();
  if (maxEntries == 0 || maxEntries > kMmsNameListEntryLimit) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS文件目录数量上限无效");
  }
  const auto commandTimeout = timeout.value_or(
      std::chrono::milliseconds(kMmsControlTimeoutMs));
  if (commandTimeout <= std::chrono::milliseconds::zero()) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS文件目录超时参数已耗尽");
  }
  {
    std::lock_guard lock(lifecycleMutex_);
    if (!started_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS会话工作器尚未启动");
    }
  }
  Channel* target = nullptr;
  {
    std::lock_guard lock(sharedState_->mutex);
    const auto active = sharedState_->activeChannel;
    if (active == IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED ||
        !sharedState_->readyChannels.contains(static_cast<int>(active))) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS当前没有已就绪的活动通道");
    }
    for (const auto& channel : channels_) {
      if (channel->channel == active) {
        target = channel.get();
        break;
      }
    }
  }
  if (target == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS活动通道不存在");
  }
  auto command = std::make_shared<ControlRequest>();
  command->kind = ControlRequest::Kind::FILE_DIRECTORY;
  command->fileDirectoryRequest = request;
  command->fileDirectoryMaxEntries = maxEntries;
  command->commandTimeout = commandTimeout;
  command->deadline = std::chrono::steady_clock::now() + commandTimeout;
  command->cancellation = std::move(cancellation);
  const auto status = SubmitControlRequest(*target, command, commandTimeout);
  if (status.ok()) {
    *entries = std::move(command->fileDirectoryResponse);
  }
  return status;
}

grpc::Status MmsSessionWorker::DownloadFile(
    const MmsFileDownloadRequest& request, MmsFileDownloadResult* result) {
  if (result == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS文件下载结果输出为空");
  }
  *result = {};
  if (request.timeout <= std::chrono::milliseconds::zero()) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS文件下载超时参数已耗尽");
  }
  {
    std::lock_guard lock(lifecycleMutex_);
    if (!started_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS会话工作器尚未启动");
    }
  }
  Channel* target = nullptr;
  {
    std::lock_guard lock(sharedState_->mutex);
    const auto active = sharedState_->activeChannel;
    if (active == IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED ||
        !sharedState_->readyChannels.contains(static_cast<int>(active))) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS当前没有已就绪的活动通道");
    }
    for (const auto& channel : channels_) {
      if (channel->channel == active) {
        target = channel.get();
        break;
      }
    }
  }
  if (target == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS活动通道不存在");
  }
  auto command = std::make_shared<ControlRequest>();
  command->kind = ControlRequest::Kind::FILE_DOWNLOAD;
  command->fileDownloadRequest = request;
  command->commandTimeout = request.timeout;
  command->deadline = std::chrono::steady_clock::now() + request.timeout;
  command->cancellation = request.cancellation;
  const auto status = SubmitControlRequest(*target, command, request.timeout);
  if (status.ok()) {
    *result = std::move(command->fileDownloadResult);
  }
  return status;
}

grpc::Status MmsSessionWorker::WriteMmsWithTimeout(
    const MmsWriteRequest& request, MmsWriteResponse* response,
    std::chrono::milliseconds timeout) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Write控制响应输出为空");
  }
  response->items.clear();
  if (request.items.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Write控制请求不能为空");
  }
  {
    std::lock_guard lock(lifecycleMutex_);
    if (!started_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS会话工作器尚未启动");
    }
  }

  Channel* target = nullptr;
  {
    std::lock_guard lock(sharedState_->mutex);
    const auto active = sharedState_->activeChannel;
    if (active == IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED ||
        !sharedState_->readyChannels.contains(static_cast<int>(active))) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS当前没有已就绪的活动通道");
    }
    for (const auto& channel : channels_) {
      if (channel->channel == active) {
        target = channel.get();
        break;
      }
    }
  }
  if (target == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS活动通道不存在");
  }

  return WriteMmsOnChannelWithTimeout(*target, request, response, timeout);
}

grpc::Status MmsSessionWorker::WriteMmsOnChannelWithTimeout(
    Channel& target, const MmsWriteRequest& request,
    MmsWriteResponse* response, std::chrono::milliseconds timeout,
    bool awaitCommandTermination, const MmsObjectName* expectedOper,
    std::uint8_t expectedControlNumber, ControlExchangeResult* exchangeResult,
    std::optional<MmsControlOperation> controlOperation,
    const MmsObjectName* controlObject,
    std::shared_ptr<std::atomic_bool> cancellation) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Write控制响应输出为空");
  }
  response->items.clear();
  if (request.items.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Write控制请求不能为空");
  }

  auto command = std::make_shared<ControlRequest>();
  command->kind = ControlRequest::Kind::WRITE;
  command->writeRequest = request;
  command->awaitCommandTermination = awaitCommandTermination;
  command->controlOperation = controlOperation;
  if (controlObject != nullptr) {
    command->controlObject = *controlObject;
  }
  command->commandTimeout = timeout;
  command->deadline = std::chrono::steady_clock::now() + timeout;
  command->cancellation = std::move(cancellation);
  if (expectedOper != nullptr) {
    command->expectedOper = *expectedOper;
  }
  command->expectedControlNumber = expectedControlNumber;
  const auto status = SubmitControlRequest(target, command, timeout);
  if (exchangeResult != nullptr) {
    std::lock_guard lock(command->mutex);
    exchangeResult->requestSent = command->requestSent;
    exchangeResult->commandTerminationReceived =
        command->commandTerminationReceived;
    exchangeResult->commandTerminationSucceeded =
        command->commandTerminationSucceeded;
    exchangeResult->commandTerminationRejected =
        command->commandTerminationRejected;
    exchangeResult->commandTerminationMalformed =
        command->commandTerminationMalformed;
    exchangeResult->commandTerminationTransportFailed =
        command->commandTerminationTransportFailed;
    exchangeResult->cancelCompleted = command->cancelCompleted;
  }
  if (status.ok()) {
    *response = std::move(command->writeResponse);
  }
  return status;
}

grpc::Status MmsSessionWorker::SubmitControlRequest(
    Channel& target, const std::shared_ptr<ControlRequest>& command,
    std::chrono::milliseconds timeout) {
  if (command == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS控制请求为空");
  }
  if (timeout <= std::chrono::milliseconds::zero()) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS控制请求等待时间已耗尽");
  }
  if (command->cancellation != nullptr &&
      command->cancellation->load(std::memory_order_acquire)) {
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS控制请求已取消");
  }
  {
    // 活动通道检查与请求入队必须持有同一组锁，防止A/B切换后把旧会话
    // 已通过能力校验的请求发送到新的物理通道。
    std::lock_guard stateLock(sharedState_->mutex);
    if (sharedState_->activeChannel ==
            IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED ||
        !sharedState_->readyChannels.contains(
            static_cast<int>(sharedState_->activeChannel))) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS当前没有已就绪的活动通道");
    }
    if (sharedState_->activeChannel != target.channel) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                          "IEC61850 MMS控制请求目标通道已切换");
    }
    std::lock_guard controlLock(target.controlMutex);
    if (!target.acceptControlRequests) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                          "IEC61850 MMS活动通道正在停止");
    }
    if (command->kind == ControlRequest::Kind::WRITE &&
        !target.supportsWrite) {
      LOG_WARNING("IEC61850 MMS Write请求被拒绝: 通道未协商Write服务, 通道={}",
                  static_cast<int>(target.channel));
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS当前通道未协商Write服务");
    }
    if (command->kind == ControlRequest::Kind::FILE_DIRECTORY &&
        !target.supportsFileDirectory) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS当前通道未协商FileDirectory服务");
    }
    if (command->kind == ControlRequest::Kind::FILE_DOWNLOAD &&
        !target.supportsFileTransfer) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          "IEC61850 MMS当前通道未协商FileOpen/FileRead/FileClose服务");
    }
    if (target.controlRequests.size() >= kMmsControlQueueLimit) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "IEC61850 MMS控制请求队列已满");
    }
    target.controlRequests.emplace_back(command);
  }
  command->condition.notify_one();

  std::unique_lock lock(command->mutex);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (command->done) {
      return command->status;
    }
    if (IsCancellationRequested(command->cancellation)) {
      // 队列中尚未发送的请求由工作线程在发送前再次检查取消标记并丢弃，
      // 调用线程不能因为前一个传输层操作阻塞而无限等待；已发送请求由会话
      // 重建隔离，避免自定义传输永久阻塞。
      command->cancelled = true;
      command->condition.notify_all();
      if (!command->requestSent) {
        LOG_DEBUG("IEC61850 MMS控制请求在队列中取消，调用线程不再等待工作线程: 通道={}, 类型={}",
                  static_cast<int>(target.channel),
                  command->kind == ControlRequest::Kind::READ
                      ? "Read"
                      : command->kind == ControlRequest::Kind::WRITE
                            ? "Write"
                            : command->kind == ControlRequest::Kind::FILE_DIRECTORY
                                  ? "FileDirectory"
                                  : "ObtainFile");
      }
      return grpc::Status(grpc::StatusCode::CANCELLED,
                          "IEC61850 MMS控制请求已取消");
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      command->cancelled = true;
      command->condition.notify_all();
      // 已进入传输层的请求不能在这里无界等待：自定义传输可能不提供
      // 可取消的Send。尚未发送的请求由工作线程在发送前检查取消标记并丢弃，
      // 调用线程直接返回，避免被前一个阻塞传输拖住。
      if (!command->requestSent) {
        LOG_DEBUG("IEC61850 MMS控制请求在队列中等待超时，调用线程不再等待工作线程: 通道={}, 类型={}",
                  static_cast<int>(target.channel),
                  command->kind == ControlRequest::Kind::READ
                      ? "Read"
                      : command->kind == ControlRequest::Kind::WRITE
                            ? "Write"
                            : command->kind == ControlRequest::Kind::FILE_DIRECTORY
                                  ? "FileDirectory"
                                  : "ObtainFile");
      }
      return grpc::Status(
          grpc::StatusCode::DEADLINE_EXCEEDED,
          command->kind == ControlRequest::Kind::READ
              ? "IEC61850 MMS Read控制请求等待超时"
              : command->kind == ControlRequest::Kind::WRITE
                    ? "IEC61850 MMS Write控制请求等待超时"
                    : command->kind == ControlRequest::Kind::FILE_DIRECTORY
                          ? "IEC61850 MMS FileDirectory请求等待超时"
                          : "IEC61850 MMS ObtainFile请求等待超时");
    }
    const auto remaining = deadline - now;
    const auto remainingMs = std::chrono::duration_cast<
        std::chrono::milliseconds>(remaining);
    const auto poll = std::min(
        std::chrono::milliseconds(10),
        std::max(std::chrono::milliseconds(1), remainingMs));
    command->condition.wait_for(lock, poll,
                                [&command] { return command->done; });
  }
}

grpc::Status MmsSessionWorker::SelectMmsControl(
    const MmsObjectName& controlObject, MmsReadResponse* response,
    std::optional<std::chrono::milliseconds> timeout,
    std::shared_ptr<std::atomic_bool> cancellation) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS SBO选择响应输出为空");
  }
  response->items.clear();
  MmsReadRequest request;
  auto status = BuildMmsControlSelectRequest(controlObject, &request);
  if (!status.ok()) {
    return status;
  }
  Channel* target = nullptr;
  std::shared_ptr<const MmsControlModel> model;
  status = ValidateControlForActiveChannel(
      controlObject, MmsControlOperation::SELECT, &target, &model);
  if (!status.ok()) {
    return status;
  }
  if (target == nullptr) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "IEC61850 MMS控制目标通道状态异常");
  }
  const auto requestTimeout = timeout.value_or(
      std::chrono::milliseconds(kMmsControlTimeoutMs));
  if (requestTimeout <= std::chrono::milliseconds::zero()) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS SBO选择等待时间已耗尽");
  }
  status = ReadMmsOnChannel(*target, request, response, requestTimeout,
                            std::move(cancellation));
  if (!status.ok()) {
    if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
      const auto uncertainStatus = target->sboState.MarkUncertain(controlObject);
      LOG_ERROR("IEC61850 MMS普通SBO选择结果不确定，已锁定控制对象: 对象={}, 原因={}",
                controlObject.identifier,
                uncertainStatus.ok() ? status.error_message()
                                     : uncertainStatus.error_message());
    }
    return status;
  }
  if (model == nullptr) {
    response->items.clear();
    LOG_ERROR("IEC61850 MMS SBO选择完成后控制能力模型丢失: 对象={}",
              controlObject.identifier);
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "IEC61850 MMS控制能力模型状态异常");
  }
  status = ValidateMmsControlSelectResponse(*response, &controlObject);
  if (!status.ok()) {
    response->items.clear();
    LOG_WARNING("IEC61850 MMS SBO选择返回值校验失败: 对象={}, 原因={}",
                controlObject.identifier, status.error_message());
    return status;
  }
  if (!IsActiveChannel(target->channel)) {
    response->items.clear();
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "IEC61850 MMS SBO选择完成时活动通道已切换");
  }
  std::optional<std::chrono::milliseconds> holdTime;
  if (const auto* capability = model->Find(controlObject);
      capability != nullptr && capability->sboTimeoutMs.has_value()) {
    holdTime = std::chrono::milliseconds(*capability->sboTimeoutMs);
  }
  status = target->sboState.RecordSelection(controlObject, NowMs(), holdTime);
  if (status.ok()) {
    LOG_INFO("IEC61850 MMS SBO选择保持已建立: 对象={}, 通道={}",
             controlObject.identifier, static_cast<int>(target->channel));
  }
  return status;
}

grpc::Status MmsSessionWorker::WriteMmsControl(
    const MmsControlCommand& command, MmsWriteResponse* response) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS控制Write响应输出为空");
  }
  response->items.clear();
  MmsWriteRequest request;
  auto status = BuildMmsControlWriteRequest(command, &request);
  if (!status.ok()) {
    return status;
  }
  Channel* target = nullptr;
  std::shared_ptr<const MmsControlModel> model;
  status = ValidateControlForActiveChannel(
      command.controlObject, command.operation, &target, &model);
  if (!status.ok()) {
    return status;
  }
  if (command.operation != MmsControlOperation::CANCEL) {
    status = ValidateMmsControlValue(*model, command.controlObject,
                                     command.controlValue);
    if (!status.ok()) {
      return status;
    }
  }
  auto requestTimeout = std::chrono::milliseconds(kMmsControlTimeoutMs);
  if (const auto* capability = model->Find(command.controlObject);
      capability != nullptr) {
    requestTimeout = ResolveMmsControlTimeout(
        *capability, command.operation, requestTimeout);
  }
  if (command.requestTimeout.has_value()) {
    if (*command.requestTimeout <= std::chrono::milliseconds::zero()) {
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                          "IEC61850 MMS控制请求等待时间已耗尽");
    }
    requestTimeout = std::min(requestTimeout, *command.requestTimeout);
  }
  LOG_DEBUG("IEC61850 MMS控制命令等待窗口: 对象={}, 操作={}, 超时={}毫秒",
            command.controlObject.identifier,
            static_cast<int>(command.operation), requestTimeout.count());
  if (target == nullptr) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "IEC61850 MMS控制目标通道状态异常");
  }
  const auto* capability = model->Find(command.controlObject);
  const bool awaitCommandTermination =
      command.operation == MmsControlOperation::OPERATE && capability != nullptr &&
      capability->ctlModel.has_value() &&
      (*capability->ctlModel == 3 || *capability->ctlModel == 4);
  const MmsObjectName* expectedOper =
      awaitCommandTermination && !request.items.empty()
          ? &request.items.front().variable
          : nullptr;
  const bool isOperate = command.operation == MmsControlOperation::OPERATE;
  bool operationReserved = false;
  if (isOperate) {
    status = target->sboState.RecordPendingOperation(command.controlObject);
    if (!status.ok()) {
      LOG_WARNING("IEC61850 MMS控制Oper无法建立执行占用: 对象={}, 原因={}",
                  command.controlObject.identifier, status.error_message());
      return status;
    }
    operationReserved = true;
  }
  ControlExchangeResult exchangeResult;
  status = WriteMmsOnChannelWithTimeout(
      *target, request, response, requestTimeout, awaitCommandTermination,
      expectedOper, command.controlNumber, &exchangeResult, command.operation,
      &command.controlObject, command.cancellation);
  const auto settleFailure = [&](const grpc::Status& failure) {
    if (isOperate && operationReserved) {
      if (exchangeResult.cancelCompleted) {
        target->sboState.ClearPendingOperation(command.controlObject);
        LOG_INFO("IEC61850 MMS增强安全Oper已由显式Cancel终止，已释放执行占用: 对象={}",
                 command.controlObject.identifier);
      } else if (!exchangeResult.requestSent) {
        target->sboState.ClearPendingOperation(command.controlObject);
        LOG_INFO("IEC61850 MMS控制Oper未发送，已释放执行占用: 对象={}",
                 command.controlObject.identifier);
      } else if (awaitCommandTermination &&
                 exchangeResult.commandTerminationRejected) {
        const bool isDirectEnhanced =
            capability != nullptr && capability->ctlModel.has_value() &&
            *capability->ctlModel == 3;
        const bool canCancelEnhancedSbo =
            capability != nullptr && capability->ctlModel.has_value() &&
            *capability->ctlModel == 4 && capability->supportsCancel;
        if (isDirectEnhanced) {
          // ctlModel=3没有SBO选择和Cancel；LastApplError已核对时结果明确。
          target->sboState.ClearPendingOperation(command.controlObject);
          LOG_WARNING(
              "IEC61850 MMS直控增强安全Oper被远端明确拒绝，已释放执行占用: 对象={}, 原因={}",
              command.controlObject.identifier, failure.error_message());
        } else if (canCancelEnhancedSbo) {
          const auto rejectedStatus =
              target->sboState.MarkOperationRejected(command.controlObject);
          LOG_WARNING(
              "IEC61850 MMS增强安全Oper被远端明确拒绝，已保留SBO并要求Cancel: 对象={}, 原因={}",
              command.controlObject.identifier,
              rejectedStatus.ok() ? failure.error_message()
                                  : rejectedStatus.error_message());
        } else {
          const auto uncertainStatus =
              target->sboState.MarkUncertain(command.controlObject);
          LOG_ERROR(
              "IEC61850 MMS增强SBO缺少可用Cancel，拒绝结果已锁定控制对象: 对象={}, 原因={}",
              command.controlObject.identifier,
              uncertainStatus.ok() ? failure.error_message()
                                   : uncertainStatus.error_message());
        }
      } else {
        const auto uncertainStatus =
            target->sboState.MarkUncertain(command.controlObject);
        LOG_ERROR("IEC61850 MMS控制Oper结果不确定，已锁定控制对象: 对象={}, 原因={}",
                  command.controlObject.identifier,
                  uncertainStatus.ok() ? failure.error_message()
                                       : uncertainStatus.error_message());
      }
    } else if (command.operation == MmsControlOperation::CANCEL &&
               exchangeResult.requestSent) {
      const auto uncertainStatus =
          target->sboState.MarkUncertain(command.controlObject);
      LOG_ERROR("IEC61850 MMS Cancel结果不确定，已锁定控制对象: 对象={}, 原因={}",
                command.controlObject.identifier,
                uncertainStatus.ok() ? failure.error_message()
                                     : uncertainStatus.error_message());
    }
    return failure;
  };
  if (!status.ok()) {
    return settleFailure(status);
  }
  const bool succeeded = response->items.size() == request.items.size() &&
                         std::ranges::all_of(response->items,
                                             [](const auto& item) {
                                               return item.success;
                                             });
  if (!succeeded) {
    LOG_WARNING(
        "IEC61850 MMS控制Write响应数量或结果无效: 对象={}, 请求项={}, 响应项={}",
        command.controlObject.identifier, request.items.size(),
        response->items.size());
    response->items.clear();
    return settleFailure(grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        "IEC61850 MMS控制Write未返回数量一致的成功结果"));
  }
  if (!IsActiveChannel(target->channel)) {
    response->items.clear();
    return settleFailure(grpc::Status(
        grpc::StatusCode::UNAVAILABLE,
        "IEC61850 MMS控制Write完成时活动通道已切换"));
  }
  switch (command.operation) {
    case MmsControlOperation::SELECT_WITH_VALUE:
      {
        std::optional<std::chrono::milliseconds> holdTime;
        if (const auto* capability = model->Find(command.controlObject);
            capability != nullptr && capability->sboTimeoutMs.has_value()) {
          holdTime =
              std::chrono::milliseconds(*capability->sboTimeoutMs);
        }
        status = target->sboState.RecordSelection(command.controlObject,
                                                  NowMs(), holdTime);
      }
      if (status.ok()) {
        LOG_INFO("IEC61850 MMS带值SBO选择保持已建立: 对象={}, 通道={}",
                 command.controlObject.identifier,
                 static_cast<int>(target->channel));
      }
      return status;
    case MmsControlOperation::OPERATE:
      if (awaitCommandTermination) {
        if (!exchangeResult.commandTerminationSucceeded) {
          return settleFailure(grpc::Status(
              grpc::StatusCode::FAILED_PRECONDITION,
              "IEC61850 MMS增强安全Oper未收到成功CommandTermination"));
        }
        target->sboState.ClearSelection(command.controlObject);
        LOG_INFO("IEC61850 MMS增强安全Oper已收到CommandTermination: 对象={}, 通道={}",
                 command.controlObject.identifier,
                 static_cast<int>(target->channel));
        return status;
      }
      target->sboState.ClearSelection(command.controlObject);
      LOG_INFO("IEC61850 MMS控制Oper已完成协议确认: 对象={}, 通道={}, 定时={}",
               command.controlObject.identifier,
               static_cast<int>(target->channel),
               command.operateTimestampMs.has_value() ? "是" : "否");
      return status;
    case MmsControlOperation::CANCEL:
      target->sboState.ClearSelection(command.controlObject);
      LOG_INFO("IEC61850 MMS控制保持已清除: 对象={}, 操作={}, 通道={}",
               command.controlObject.identifier,
               static_cast<int>(command.operation),
               static_cast<int>(target->channel));
      return status;
    case MmsControlOperation::SELECT:
      break;
  }
  return status;
}

grpc::Status MmsSessionWorker::ExecuteMmsPointControl(
    const MmsPointControlCommand& pointCommand,
    MmsWriteResponse* response) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS同步控制响应输出为空");
  }
  response->items.clear();

  MmsControlCapability capability;
  auto status = GetActiveControlCapability(pointCommand.controlObject,
                                           &capability);
  if (!status.ok()) {
    return status;
  }

  const auto controlStart = std::chrono::steady_clock::now();
  const auto deadline = pointCommand.requestTimeout.has_value()
                            ? controlStart + *pointCommand.requestTimeout
                            : std::chrono::steady_clock::time_point::max();
  const auto remainingTimeout = [&]()
      -> std::optional<std::chrono::milliseconds> {
    if (!pointCommand.requestTimeout.has_value()) {
      return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return std::chrono::milliseconds::zero();
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    return std::max(std::chrono::milliseconds(1), remaining);
  };
  if (pointCommand.requestTimeout.has_value() &&
      *pointCommand.requestTimeout <= std::chrono::milliseconds::zero()) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS同步控制截止时间已到");
  }

  bool selectionEstablished = false;
  const auto clearSelection = [&](std::string_view cause) {
    Channel* target = nullptr;
    std::shared_ptr<const MmsControlModel> model;
    const auto clearStatus = ValidateControlForActiveChannel(
        pointCommand.controlObject, MmsControlOperation::OPERATE, &target,
        &model);
    if (clearStatus.ok() && target != nullptr) {
      target->sboState.ClearSelection(pointCommand.controlObject);
      LOG_WARNING("IEC61850 MMS同步控制{}，已清除本地SBO保持: 对象={}", cause,
                  pointCommand.controlObject.identifier);
    } else {
      LOG_ERROR("IEC61850 MMS同步控制{}，清除本地SBO保持失败: 对象={}, 原因={}",
                cause, pointCommand.controlObject.identifier,
                clearStatus.error_message());
    }
  };

  MmsControlCommand command;
  command.controlObject = pointCommand.controlObject;
  command.controlNumber = AllocateControlNumber();
  command.originCategory = 2;
  command.timestampMs = NowMs();
  command.test = false;
  command.check = 0;
  command.cancellation = pointCommand.cancellation;
  status = EncodeMmsPointControlValue(pointCommand, capability,
                                      &command.controlValue);
  if (!status.ok()) {
    return status;
  }

  if (capability.supportsSbo) {
    MmsReadResponse selectResponse;
    const auto timeout = remainingTimeout();
    if (timeout.has_value() && *timeout <= std::chrono::milliseconds::zero()) {
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                          "IEC61850 MMS同步控制SBO选择截止时间已到");
    }
    status = SelectMmsControl(command.controlObject, &selectResponse, timeout,
                              pointCommand.cancellation);
    if (!status.ok()) {
      return status;
    }
    selectionEstablished = true;
  } else if (capability.supportsSboWithValue) {
    command.operation = MmsControlOperation::SELECT_WITH_VALUE;
    command.requestTimeout = remainingTimeout();
    MmsWriteResponse selectResponse;
    status = WriteMmsControl(command, &selectResponse);
    if (!status.ok()) {
      return status;
    }
    selectionEstablished = true;
  }

  if (selectionEstablished &&
      IsCancellationRequested(pointCommand.cancellation)) {
    clearSelection("请求已取消");
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS同步控制已取消");
  }
  command.operation = MmsControlOperation::OPERATE;
  command.requestTimeout = remainingTimeout();
  if (command.requestTimeout.has_value() &&
      *command.requestTimeout <= std::chrono::milliseconds::zero()) {
    if (selectionEstablished) {
      clearSelection("截止时间耗尽");
    }
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS同步控制Oper截止时间已到");
  }
  return WriteMmsControl(command, response);
}

grpc::Status MmsSessionWorker::ReadSettingGroupStatus(
    const MmsSettingGroupPlan& plan, MmsSettingGroupStatus* status) {
  MmsSettingGroupClient client(
      [this](const MmsReadRequest& request, MmsReadResponse* response) {
        return ReadMms(request, response);
      },
      [this](const MmsWriteRequest& request, MmsWriteResponse* response) {
        return WriteMms(request, response);
      });
  return client.ReadStatus(plan, status);
}

grpc::Status MmsSessionWorker::SelectSettingGroup(
    const MmsSettingGroupPlan& plan, std::uint32_t group) {
  MmsSettingGroupClient client(
      [this](const MmsReadRequest& request, MmsReadResponse* response) {
        return ReadMms(request, response);
      },
      [this](const MmsWriteRequest& request, MmsWriteResponse* response) {
        return WriteMms(request, response);
      });
  return client.Select(plan, group);
}

grpc::Status MmsSessionWorker::ConfirmSettingGroupEdit(
    const MmsSettingGroupPlan& plan) {
  MmsSettingGroupClient client(
      [this](const MmsReadRequest& request, MmsReadResponse* response) {
        return ReadMms(request, response);
      },
      [this](const MmsWriteRequest& request, MmsWriteResponse* response) {
        return WriteMms(request, response);
      });
  return client.ConfirmEdit(plan);
}

grpc::Status MmsSessionWorker::CancelSettingGroupEdit(
    const MmsSettingGroupPlan& plan) {
  MmsSettingGroupClient client(
      [this](const MmsReadRequest& request, MmsReadResponse* response) {
        return ReadMms(request, response);
      },
      [this](const MmsWriteRequest& request, MmsWriteResponse* response) {
        return WriteMms(request, response);
      });
  return client.CancelEdit(plan);
}

grpc::Status MmsSessionWorker::ActivateSettingGroup(
    const MmsSettingGroupPlan& plan, std::uint32_t group) {
  MmsSettingGroupClient client(
      [this](const MmsReadRequest& request, MmsReadResponse* response) {
        return ReadMms(request, response);
      },
      [this](const MmsWriteRequest& request, MmsWriteResponse* response) {
        return WriteMms(request, response);
      });
  return client.Activate(plan, group);
}

std::shared_ptr<MmsSessionWorker::ControlRequest>
MmsSessionWorker::TakeControlRequest(Channel& channel) {
  std::lock_guard lock(channel.controlMutex);
  if (channel.controlRequests.empty()) {
    return nullptr;
  }
  auto request = std::move(channel.controlRequests.front());
  channel.controlRequests.pop_front();
  return request;
}

std::shared_ptr<MmsSessionWorker::ControlRequest>
MmsSessionWorker::TakePendingCancelRequest(
    Channel& channel, const MmsObjectName& controlObject) {
  std::lock_guard lock(channel.controlMutex);
  for (auto it = channel.controlRequests.begin();
       it != channel.controlRequests.end(); ++it) {
    const auto& candidate = *it;
    if (candidate == nullptr || !candidate->controlOperation.has_value() ||
        *candidate->controlOperation != MmsControlOperation::CANCEL ||
        !SameObjectName(candidate->controlObject, controlObject)) {
      continue;
    }
    auto request = std::move(*it);
    channel.controlRequests.erase(it);
    return request;
  }
  return nullptr;
}

void MmsSessionWorker::CompleteControlRequest(
    const std::shared_ptr<ControlRequest>& request,
    const grpc::Status& status) {
  if (request == nullptr) {
    return;
  }
  {
    std::lock_guard lock(request->mutex);
    if (request->done) {
      return;
    }
    request->status = status;
    request->done = true;
  }
  request->condition.notify_all();
}

void MmsSessionWorker::CancelControlRequests(Channel& channel,
                                              grpc::Status status) {
  std::deque<std::shared_ptr<ControlRequest>> requests;
  {
    std::lock_guard lock(channel.controlMutex);
    requests.swap(channel.controlRequests);
  }
  for (const auto& request : requests) {
    CompleteControlRequest(request, status);
  }
}

bool MmsSessionWorker::IsActiveChannel(
    IEC61850Proto::NetworkChannel channel) const {
  std::lock_guard lock(sharedState_->mutex);
  return sharedState_->activeChannel == channel &&
         sharedState_->readyChannels.contains(static_cast<int>(channel));
}

grpc::Status MmsSessionWorker::ValidateControlForActiveChannel(
    const MmsObjectName& controlObject, MmsControlOperation operation,
    Channel** target, std::shared_ptr<const MmsControlModel>* model) {
  if (target == nullptr || model == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS控制通道输出参数为空");
  }
  *target = nullptr;
  model->reset();
  {
    std::lock_guard lock(lifecycleMutex_);
    if (!started_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS会话工作器尚未启动");
    }
  }

  const auto active = [&] {
    std::lock_guard lock(sharedState_->mutex);
    return sharedState_->activeChannel;
  }();
  if (active == IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS当前没有已就绪的活动通道");
  }
  for (const auto& channel : channels_) {
    if (channel->channel != active) {
      continue;
    }
    std::shared_ptr<const MmsControlModel> currentModel;
    {
      std::lock_guard lock(channel->controlMutex);
      if (!channel->acceptControlRequests) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "IEC61850 MMS活动通道正在停止");
      }
      currentModel = channel->controlModel;
    }
    if (currentModel == nullptr) {
      LOG_WARNING("IEC61850 MMS专用控制请求被拒绝: 对象={}, 原因=在线控制能力模型不存在",
                  controlObject.identifier);
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS在线控制能力模型不存在");
    }
    const auto status = ValidateMmsControlOperation(
        *currentModel, controlObject, operation, channel->sboState, NowMs());
    if (!status.ok()) {
      LOG_WARNING("IEC61850 MMS专用控制请求被拒绝: 对象={}, 原因={}",
                  controlObject.identifier, status.error_message());
      return status;
    }
    *target = channel.get();
    *model = std::move(currentModel);
    return grpc::Status::OK;
  }
  return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                      "IEC61850 MMS活动通道不存在");
}

grpc::Status MmsSessionWorker::GetActiveControlCapability(
    const MmsObjectName& controlObject, MmsControlCapability* capability) {
  if (capability == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS控制能力输出参数为空");
  }
  *capability = {};
  {
    std::lock_guard lock(lifecycleMutex_);
    if (!started_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS会话工作器尚未启动");
    }
  }

  IEC61850Proto::NetworkChannel active =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  {
    std::lock_guard lock(sharedState_->mutex);
    active = sharedState_->activeChannel;
  }
  if (active == IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS当前没有已就绪的活动通道");
  }
  for (const auto& channel : channels_) {
    if (channel->channel != active) {
      continue;
    }
    std::lock_guard lock(channel->controlMutex);
    if (!channel->acceptControlRequests) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                          "IEC61850 MMS活动通道正在停止");
    }
    if (channel->controlModel == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS在线控制能力模型不存在");
    }
    const auto* found = channel->controlModel->Find(controlObject);
    if (found == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "控制对象不在在线能力模型中");
    }
    *capability = *found;
    return grpc::Status::OK;
  }
  return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                      "IEC61850 MMS活动通道不存在");
}

std::uint8_t MmsSessionWorker::AllocateControlNumber() {
  std::lock_guard lock(controlNumberMutex_);
  const auto value = nextControlNumber_;
  nextControlNumber_ = nextControlNumber_ >= 255 ? 1 : nextControlNumber_ + 1;
  return static_cast<std::uint8_t>(value);
}

grpc::Status MmsSessionWorker::WaitForCommandTermination(
    Channel& channel, const std::shared_ptr<ControlRequest>& request,
    std::stop_token stopToken, std::chrono::milliseconds timeout) {
  if (request == nullptr || !request->awaitCommandTermination ||
      request->expectedOper.identifier.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMSCommandTermination等待参数无效");
  }
  const auto updateResult = [&](auto&& update) {
    std::lock_guard lock(request->mutex);
    update(*request);
  };
  const auto reportPlans = BuildMmsReportDecodePlans(plan_);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto consume = [&](std::span<const std::uint8_t> mmsPdu)
      -> std::optional<grpc::Status> {
    bool commandTerminationCandidate = false;
    if (!mmsPdu.empty() && mmsPdu.front() == 0xa3) {
      std::size_t outerOffset = 0;
      BerTlvView outer;
      if (ReadBerTlv(mmsPdu, &outerOffset, &outer).ok() &&
          outerOffset == mmsPdu.size() && outer.tag == 0xa3) {
        std::size_t reportOffset = 0;
        BerTlvView report;
        if (ReadBerTlv(outer.value, &reportOffset, &report).ok() &&
            report.tag == 0xa0) {
          std::size_t variableOffset = 0;
          BerTlvView variableAccess;
          if (ReadBerTlv(report.value, &variableOffset, &variableAccess).ok() &&
              variableAccess.tag == 0xa0) {
            commandTerminationCandidate = true;
          }
        }
      }
    }
    if (!commandTerminationCandidate) {
      ProcessMmsInformationReport(channel, mmsPdu, reportPlans);
      return std::nullopt;
    }
    MmsCommandTermination termination;
    const auto status = DecodeMmsCommandTermination(
        mmsPdu, request->expectedOper, request->expectedControlNumber,
        &termination);
    if (!status.ok()) {
      updateResult([](auto& current) {
        current.commandTerminationReceived = true;
        current.commandTerminationMalformed = true;
      });
      LOG_WARNING("IEC61850 MMS CommandTermination解码失败: 通道={}, 原因={}, 报文={}",
                  static_cast<int>(channel.channel), status.error_message(),
                  HexDump(mmsPdu));
      return status;
    }
    if (termination.success) {
      updateResult([](auto& current) {
        current.commandTerminationReceived = true;
        current.commandTerminationSucceeded = true;
      });
      LOG_INFO("IEC61850 MMS CommandTermination确认控制完成: 通道={}, 对象={}",
               static_cast<int>(channel.channel),
               request->expectedOper.identifier);
      return grpc::Status::OK;
    }
    if (!termination.lastApplError.has_value()) {
      updateResult([](auto& current) {
        current.commandTerminationReceived = true;
        current.commandTerminationMalformed = true;
      });
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          "IEC61850 MMS CommandTermination未提供LastApplError");
    }
    const auto& error = *termination.lastApplError;
    updateResult([](auto& current) {
      current.commandTerminationReceived = true;
      current.commandTerminationRejected = true;
    });
    LOG_WARNING(
        "IEC61850 MMS CommandTermination报告LastApplError: 通道={}, 对象={}, Error={}, AddCause={}, ctlNum={}",
        static_cast<int>(channel.channel), request->expectedOper.identifier,
        error.error, error.addCause, error.controlNumber);
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("IEC61850 MMS控制终止失败: Error={}, AddCause={}, ctlNum={}",
                    error.error, error.addCause, error.controlNumber));
  };

  for (const auto& pdu : request->unconfirmedDuringControl) {
    if (const auto status = consume(pdu); status.has_value()) {
      return *status;
    }
  }
  request->unconfirmedDuringControl.clear();

  for (;;) {
    if (stopToken.stop_requested()) {
      return grpc::Status(grpc::StatusCode::CANCELLED,
                          "IEC61850 MMSCommandTermination等待已取消");
    }
    {
      std::lock_guard lock(request->mutex);
      if (request->cancelled) {
        return grpc::Status(grpc::StatusCode::CANCELLED,
                            "IEC61850 MMSCommandTermination等待已取消");
      }
    }
    if (auto cancelRequest =
            TakePendingCancelRequest(channel, request->controlObject);
        cancelRequest != nullptr) {
      {
        std::lock_guard controlLock(channel.controlMutex);
        channel.controlRequests.emplace_front(cancelRequest);
      }
      ProcessControlRequests(channel, stopToken);
      grpc::Status cancelStatus;
      {
        std::lock_guard cancelLock(cancelRequest->mutex);
        cancelStatus = cancelRequest->status;
      }
      if (cancelStatus.ok()) {
        updateResult([](auto& current) { current.cancelCompleted = true; });
        LOG_INFO("IEC61850 MMS增强安全Oper等待期间已完成显式Cancel: 通道={}, 对象={}",
                 static_cast<int>(channel.channel),
                 request->expectedOper.identifier);
        return grpc::Status(grpc::StatusCode::CANCELLED,
                            "IEC61850 MMS控制Oper已由显式Cancel终止");
      }
      LOG_WARNING("IEC61850 MMS增强安全Oper等待期间Cancel未完成: 通道={}, 原因={}",
                  static_cast<int>(channel.channel),
                  cancelStatus.error_message());
    }
    if (!IsActiveChannel(channel.channel)) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                          "IEC61850 MMSCommandTermination等待期间活动通道已切换");
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return grpc::Status(
          grpc::StatusCode::DEADLINE_EXCEEDED,
          "IEC61850 MMS等待CommandTermination超时");
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    const auto receiveTimeout = static_cast<std::uint32_t>(std::min<std::int64_t>(
        kMmsCommandTerminationPollMs, std::max<std::int64_t>(1, remaining.count())));
    std::vector<std::uint8_t> received;
    auto status = channel.transport->Receive(&received, receiveTimeout);
    if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
      continue;
    }
    if (!status.ok()) {
      updateResult([](auto& current) {
        current.commandTerminationTransportFailed = true;
      });
      return status;
    }
    IsoSessionPduView sessionPdu;
    status = DecodeIsoSessionPdu(received, &sessionPdu);
    if (!status.ok()) {
      updateResult([](auto& current) {
        current.commandTerminationMalformed = true;
      });
      return status;
    }
    if (sessionPdu.type == IsoSessionPduType::FINISH ||
        sessionPdu.type == IsoSessionPduType::DISCONNECT ||
        sessionPdu.type == IsoSessionPduType::ABORT) {
      updateResult([](auto& current) {
        current.commandTerminationTransportFailed = true;
      });
      return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                          "IEC61850 MMSCommandTermination等待期间会话已关闭");
    }
    if (sessionPdu.type != IsoSessionPduType::DATA) {
      continue;
    }
    std::span<const std::uint8_t> mmsPdu;
    status = DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu);
    if (!status.ok()) {
      updateResult([](auto& current) {
        current.commandTerminationMalformed = true;
      });
      return status;
    }
    if (const auto completion = consume(mmsPdu); completion.has_value()) {
      return *completion;
    }
  }
}

void MmsSessionWorker::ProcessControlRequests(Channel& channel,
                                              std::stop_token stopToken) {
  for (;;) {
    auto command = TakeControlRequest(channel);
    if (command == nullptr) {
      return;
    }
    const auto requestCancelled = [command, stopToken] {
      if (stopToken.stop_requested()) {
        return true;
      }
      if (IsCancellationRequested(command->cancellation)) {
        return true;
      }
      std::lock_guard lock(command->mutex);
      return command->cancelled;
    };
    if (requestCancelled()) {
      CompleteControlRequest(
          command, grpc::Status(grpc::StatusCode::CANCELLED,
                                "IEC61850 MMS控制请求已取消"));
      continue;
    }
    if (stopToken.stop_requested()) {
      CompleteControlRequest(
          command, grpc::Status(grpc::StatusCode::CANCELLED,
                                "IEC61850 MMS控制请求已取消"));
      continue;
    }
    if (channel.nextInvokeId == 0) {
      CompleteControlRequest(
          command, grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                "IEC61850 MMS控制请求invokeID耗尽"));
      continue;
    }

    const bool isFileRequest =
        command->kind == ControlRequest::Kind::FILE_DIRECTORY ||
        command->kind == ControlRequest::Kind::FILE_DOWNLOAD;
    const auto invokeId = channel.nextInvokeId;
    auto status = isFileRequest
                      ? grpc::Status::OK
                      : AdvanceInvokeId(&channel.nextInvokeId);
    if (!status.ok()) {
      CompleteControlRequest(command, status);
      continue;
    }
    std::array<std::uint8_t, kMmsPduBufferSize> requestBuffer{};
    std::size_t requestSize = 0;
    std::vector<std::uint8_t> response;
    const auto enqueueUnconfirmed =
        [this, &command, channelId = channel.channel](
            std::span<const std::uint8_t> pdu) {
          if (command->awaitCommandTermination) {
            command->unconfirmedDuringControl.emplace_back(pdu.begin(),
                                                            pdu.end());
            LOG_DEBUG("IEC61850 MMS控制交换暂存未确认PDU等待CommandTermination: 通道={}, 报文={}",
                      static_cast<int>(channelId), HexDump(pdu));
            return;
          }
          EnqueueUnconfirmed(channelId, pdu);
        };
    if (command->kind == ControlRequest::Kind::READ) {
      status = EncodeMmsReadRequest(invokeId, command->readRequest,
                                    requestBuffer, &requestSize);
    } else if (command->kind == ControlRequest::Kind::WRITE) {
      status = EncodeMmsWriteRequest(invokeId, command->writeRequest,
                                     requestBuffer, &requestSize);
    }
    if (status.ok()) {
      if (requestCancelled()) {
        LOG_INFO("IEC61850 MMS控制请求在发送前已取消: 通道={}, 类型={}",
                 static_cast<int>(channel.channel),
                 command->kind == ControlRequest::Kind::READ
                     ? "Read"
                     : command->kind == ControlRequest::Kind::WRITE
                           ? "Write"
                           : command->kind == ControlRequest::Kind::FILE_DIRECTORY
                                 ? "FileDirectory"
                                 : "ObtainFile");
        CompleteControlRequest(
            command, grpc::Status(grpc::StatusCode::CANCELLED,
                                  "IEC61850 MMS控制请求已取消"));
        continue;
      }
      const auto remainingCommandTimeout = [&]() {
        const auto now = std::chrono::steady_clock::now();
        if (now >= command->deadline) {
          return std::chrono::milliseconds::zero();
        }
        return std::max(
            std::chrono::milliseconds(1),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                command->deadline - now));
      };
      const auto exchangeTimeout = remainingCommandTimeout();
      if (exchangeTimeout <= std::chrono::milliseconds::zero()) {
        status = grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                              "IEC61850 MMS控制请求截止时间已耗尽");
      } else {
        const auto markRequestSent = [command] {
          std::lock_guard requestLock(command->mutex);
          if (command->cancelled ||
              IsCancellationRequested(command->cancellation)) {
            return false;
          }
          command->requestSent = true;
          return true;
        };
        if (isFileRequest) {
          MmsFileClient fileClient;
          const auto fileTimeout = remainingCommandTimeout();
          if (fileTimeout <= std::chrono::milliseconds::zero()) {
            status = grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                  "IEC61850 MMS文件请求截止时间已耗尽");
          } else if (command->kind == ControlRequest::Kind::FILE_DIRECTORY) {
            MmsFileDirectoryResponse directoryResponse;
            status = fileClient.ReadDirectory(
                *channel.transport, &channel.nextInvokeId,
                command->fileDirectoryRequest, &directoryResponse, fileTimeout,
                enqueueUnconfirmed, requestCancelled, markRequestSent);
            if (status.ok() &&
                directoryResponse.entries.size() >
                    command->fileDirectoryMaxEntries) {
              status = grpc::Status(
                  grpc::StatusCode::RESOURCE_EXHAUSTED,
                  "IEC61850 MMS文件目录结果超过调用方数量上限");
            }
            if (status.ok()) {
              command->fileDirectoryResponse =
                  std::move(directoryResponse.entries);
            }
          } else {
            auto fileRequest = command->fileDownloadRequest;
            if (fileRequest.remoteFile.empty()) {
              fileRequest.remoteFile = fileRequest.remoteFileName;
            }
            if (fileRequest.localFile.empty()) {
              fileRequest.localFile = fileRequest.localPath;
            }
            fileRequest.timeout = fileTimeout;
            status = fileClient.Download(*channel.transport,
                                         &channel.nextInvokeId, fileRequest,
                                         fileTimeout, enqueueUnconfirmed,
                                         requestCancelled, markRequestSent);
            if (status.ok()) {
              std::error_code fileSizeError;
              const auto localFileSize = std::filesystem::file_size(
                  fileRequest.localFile, fileSizeError);
              if (fileSizeError) {
                status = grpc::Status(
                    grpc::StatusCode::INTERNAL,
                    std::format("IEC61850 MMS下载结果文件统计失败: {}",
                                fileSizeError.message()));
              } else {
                command->fileDownloadResult.bytesWritten = localFileSize;
                command->fileDownloadResult.localPath = fileRequest.localFile;
              }
            }
          }
        } else {
          status = ExchangeMmsConfirmedRequest(
              *channel.transport,
              std::span<const std::uint8_t>(requestBuffer.data(), requestSize),
              &response, enqueueUnconfirmed, exchangeTimeout,
              requestCancelled, markRequestSent);
        }
      }
    }
    if (status.ok()) {
      if (requestCancelled()) {
        status = grpc::Status(grpc::StatusCode::CANCELLED,
                              "IEC61850 MMS控制请求已取消");
      }
    }
    if (status.ok()) {
      if (command->kind == ControlRequest::Kind::READ) {
        status = DecodeMmsReadResponse(response, invokeId,
                                       &command->readResponse);
      } else if (command->kind == ControlRequest::Kind::WRITE) {
        status = DecodeMmsWriteResponse(response, invokeId,
                                        &command->writeResponse);
        if (status.ok() && command->awaitCommandTermination) {
          const auto now = std::chrono::steady_clock::now();
          if (now >= command->deadline) {
            status = grpc::Status(
                grpc::StatusCode::DEADLINE_EXCEEDED,
                "IEC61850 MMS控制CommandTermination截止时间已耗尽");
          } else {
            const auto remaining = std::max(
                std::chrono::milliseconds(1),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    command->deadline - now));
            status = WaitForCommandTermination(channel, command, stopToken,
                                               remaining);
          }
        }
      }
    }
    if (status.ok() && requestCancelled()) {
      status = grpc::Status(grpc::StatusCode::CANCELLED,
                            "IEC61850 MMS控制请求已取消");
    }
    bool requestSent = false;
    bool cancelCompleted = false;
    {
      std::lock_guard requestLock(command->mutex);
      requestSent = command->requestSent;
      cancelCompleted = command->cancelCompleted;
    }
    if (!status.ok() && requestSent && !cancelCompleted &&
        (status.error_code() == grpc::StatusCode::CANCELLED ||
         status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED)) {
      // 已发送但结果未知的请求不能留在原会话中；关闭当前会话，交由Run
      // 统一清理状态并按完整目录/RCB/GI流程重建，避免旧响应污染后续请求。
      LOG_WARNING("IEC61850 MMS控制请求结果不确定，关闭并重建当前会话: 通道={}, 原因={}",
                  static_cast<int>(channel.channel), status.error_message());
      channel.transport->Close();
    }
    if (!status.ok()) {
      LOG_WARNING("IEC61850 MMS控制请求失败: 通道={}, 类型={}, 原因={}",
                  static_cast<int>(channel.channel),
                  command->kind == ControlRequest::Kind::READ
                      ? "Read"
                      : command->kind == ControlRequest::Kind::WRITE
                            ? "Write"
                            : command->kind == ControlRequest::Kind::FILE_DIRECTORY
                                  ? "FileDirectory"
                                  : "ObtainFile",
                  status.error_message());
    } else {
      LOG_INFO("IEC61850 MMS控制请求完成: 通道={}, 类型={}, invokeID={}",
               static_cast<int>(channel.channel),
               command->kind == ControlRequest::Kind::READ
                   ? "Read"
                   : command->kind == ControlRequest::Kind::WRITE
                         ? "Write"
                         : command->kind == ControlRequest::Kind::FILE_DIRECTORY
                               ? "FileDirectory"
                               : "ObtainFile",
               invokeId);
    }
    CompleteControlRequest(command, status);
  }
}

grpc::Status MmsSessionWorker::Start() {
  std::lock_guard lock(lifecycleMutex_);
  if (started_) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS会话工作器已经启动");
  }
  if (channels_.empty()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS没有可用网络通道");
  }
  std::vector<IEC61850Proto::NetworkChannel> seen;
  for (const auto& channel : channels_) {
    if (channel->channel == IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED ||
        std::find(seen.begin(), seen.end(), channel->channel) != seen.end()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "IEC61850 MMS网络通道标识重复或无效");
    }
    if (channel->endpoint.remoteIp.empty() ||
        channel->endpoint.remotePort == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "IEC61850 MMS网络通道缺少远端地址");
    }
    seen.emplace_back(channel->channel);
  }
  try {
    for (const auto& channel : channels_) {
      if (channel->transport != nullptr) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "IEC61850 MMS通道传输对象仍在使用");
      }
      if (transportFactory_) {
        channel->transport = transportFactory_(channel->endpoint,
                                               channel->channel);
      } else {
        channel->transport = std::make_unique<MmsTcpTransport>();
      }
      if (channel->transport == nullptr) {
        LOG_ERROR("IEC61850 MMS创建传输对象返回空指针: 通道={}",
                  static_cast<int>(channel->channel));
        for (const auto& created : channels_) {
          if (created->transport != nullptr) {
            created->transport->Close();
            created->transport.reset();
          }
        }
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "IEC61850 MMS创建传输对象失败");
      }
    }
  } catch (const std::exception& exception) {
    LOG_ERROR("IEC61850 MMS创建传输对象发生异常: {}", exception.what());
    for (const auto& channel : channels_) {
      if (channel->transport != nullptr) {
        channel->transport->Close();
        channel->transport.reset();
      }
    }
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "IEC61850 MMS创建传输对象发生异常");
  } catch (...) {
    LOG_ERROR("IEC61850 MMS创建传输对象发生未知异常");
    for (const auto& channel : channels_) {
      if (channel->transport != nullptr) {
        channel->transport->Close();
        channel->transport.reset();
      }
    }
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "IEC61850 MMS创建传输对象发生未知异常");
  }
  started_ = true;
  {
    std::lock_guard stateLock(sharedState_->mutex);
    sharedState_->pendingUnconfirmed.clear();
    sharedState_->pendingUnconfirmedBytes = 0;
    sharedState_->rcbReconfigurationRequests.clear();
    sharedState_->rcbConfigurationChannel.reset();
    sharedState_->activeChannel =
        IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
    sharedState_->readyChannels.clear();
    for (auto& status : sharedState_->channels) {
      status.state = IEC61850Proto::CHANNEL_STATE_CONNECTING;
      status.error.clear();
    }
  }
  for (const auto& channel : channels_) {
    std::lock_guard controlLock(channel->controlMutex);
    channel->controlRequests.clear();
    channel->controlModel.reset();
    channel->nextInvokeId = 1;
      channel->acceptControlRequests = true;
      channel->supportsWrite = false;
      channel->supportsFileDirectory = false;
      channel->supportsFileTransfer = false;
    channel->sboState.Clear();
  }
  PublishState(MmsConnectionEventType::STATE_SNAPSHOT,
               ProtocolSessionState::CONNECTING, 0, {});
  try {
    workers_.reserve(channels_.size());
    for (std::size_t index = 0; index < channels_.size(); ++index) {
      workers_.emplace_back([this, index](std::stop_token stopToken) {
        Run(index, stopToken);
      });
    }
  } catch (const std::exception& exception) {
    LOG_ERROR("IEC61850 MMS创建会话线程失败: {}", exception.what());
    for (auto& worker : workers_) {
      worker.request_stop();
    }
    workers_.clear();
    for (const auto& channel : channels_) {
      if (channel->transport != nullptr) {
        channel->transport->Close();
        channel->transport.reset();
      }
    }
    started_ = false;
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "IEC61850 MMS创建会话线程失败");
  }
  LOG_INFO("IEC61850 MMS自研会话工作器已启动: 通道数量={}", channels_.size());
  return grpc::Status::OK;
}

void MmsSessionWorker::Stop() noexcept {
  std::lock_guard lock(lifecycleMutex_);
  if (!started_ && workers_.empty()) {
    return;
  }
  for (const auto& channel : channels_) {
    {
      std::lock_guard controlLock(channel->controlMutex);
      channel->acceptControlRequests = false;
      channel->supportsWrite = false;
      channel->supportsFileDirectory = false;
      channel->supportsFileTransfer = false;
    }
    CancelControlRequests(
        *channel,
        grpc::Status(grpc::StatusCode::CANCELLED,
                     "IEC61850 MMS会话工作器正在停止"));
  }
  for (auto& worker : workers_) {
    worker.request_stop();
  }
  workers_.clear();
  for (auto& channel : channels_) {
    if (channel->transport != nullptr) {
      channel->transport->Close();
      channel->transport.reset();
    }
  }
  started_ = false;
  LOG_INFO("IEC61850 MMS自研会话工作器已停止");
}

grpc::Status MmsSessionWorker::Establish(Channel& channel,
                                         std::stop_token stopToken) {
  if (stopToken.stop_requested()) {
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS会话建立已取消");
  }
  if (channel.transport == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS传输对象创建失败");
  }
  const auto associationBudgetMs =
      DefaultMmsAssociationBudgetMs(channel.endpoint);
  if (associationBudgetMs == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS关联建立超时参数无效");
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(associationBudgetMs);
  auto remainingTimeout = RemainingTimeoutMs(deadline);
  if (remainingTimeout == 0) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS关联建立截止时间已耗尽");
  }
  auto status = channel.transport->Connect(channel.endpoint, remainingTimeout);
  if (!status.ok()) {
    return status;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    LOG_WARNING("IEC61850 MMS TCP/COTP建链返回时已超过关联总截止时间: 通道={}",
                static_cast<int>(channel.channel));
    channel.transport->Close();
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS TCP/COTP建链超过关联总截止时间");
  }
  if (stopToken.stop_requested()) {
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS会话建立已取消");
  }

  // 通用WriteMms是受控运行时入口，因此即使当前静态计划不强制Write，
  // Initiate也始终提出该能力；服务端是否接受按物理会话单独保存。
  const auto request = DefaultInitiateRequest(true);
  std::array<std::uint8_t, kMmsPduBufferSize> initiateBuffer{};
  std::size_t initiateSize = 0;
  status = EncodeMmsInitiateRequest(request, initiateBuffer, &initiateSize);
  if (!status.ok()) {
    return status;
  }
  std::array<std::uint8_t, kMmsPduBufferSize> aarqBuffer{};
  std::size_t aarqSize = 0;
  status = EncodeMmsAarq(kMmsApplicationContextOid,
                         std::span<const std::uint8_t>(initiateBuffer.data(),
                                                       initiateSize),
                         aarqBuffer, &aarqSize);
  if (!status.ok()) {
    return status;
  }
  std::array<std::uint8_t, kMmsPduBufferSize> connectBuffer{};
  std::size_t connectSize = 0;
  status = EncodeIsoSessionConnect(
      std::span<const std::uint8_t>(aarqBuffer.data(), aarqSize), connectBuffer,
      &connectSize);
  if (!status.ok()) {
    return status;
  }
  remainingTimeout = RemainingTimeoutMs(deadline);
  if (remainingTimeout == 0) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS Session CONNECT截止时间已耗尽");
  }
  status = channel.transport->Send(
      std::span<const std::uint8_t>(connectBuffer.data(), connectSize),
      remainingTimeout);
  if (!status.ok()) {
    return status;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    LOG_WARNING("IEC61850 MMS Session CONNECT发送返回时已超过关联总截止时间: 通道={}",
                static_cast<int>(channel.channel));
    channel.transport->Close();
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS Session CONNECT超过关联总截止时间");
  }

  std::vector<std::uint8_t> received;
  remainingTimeout = RemainingTimeoutMs(deadline);
  if (remainingTimeout == 0) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS Session ACCEPT截止时间已耗尽");
  }
  status = channel.transport->Receive(&received, remainingTimeout);
  if (!status.ok()) {
    return status;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    LOG_WARNING("IEC61850 MMS Session ACCEPT接收返回时已超过关联总截止时间: 通道={}",
                static_cast<int>(channel.channel));
    channel.transport->Close();
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS Session ACCEPT超过关联总截止时间");
  }
  IsoSessionPduView sessionPdu;
  status = DecodeIsoSessionPdu(received, &sessionPdu);
  if (!status.ok()) {
    return status;
  }
  if (sessionPdu.type != IsoSessionPduType::ACCEPT) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS服务端未返回Session ACCEPT");
  }
  MmsAareView aare;
  status = DecodeMmsAare(sessionPdu.userData, &aare);
  if (!status.ok()) {
    return status;
  }
  if (!IsSameOid(aare, kMmsApplicationContextOid)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS AARE应用上下文不匹配");
  }
  if (aare.result != 0 || aare.mmsPdu.empty()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        std::format("IEC61850 MMS关联被拒绝: result={}",
                                    aare.result));
  }
  MmsInitiateResponse response;
  status = DecodeMmsInitiateResponse(aare.mmsPdu, &response);
  if (!status.ok()) {
    return status;
  }
  if ((response.hasNegotiatedMaxServOutstandingCalling &&
       response.negotiatedMaxServOutstandingCalling >
           request.proposedMaxServOutstandingCalling) ||
      (response.hasNegotiatedMaxServOutstandingCalled &&
       response.negotiatedMaxServOutstandingCalled >
           request.proposedMaxServOutstandingCalled) ||
      (response.hasNegotiatedDataStructureNestingLevel &&
       response.negotiatedDataStructureNestingLevel >
           request.proposedDataStructureNestingLevel)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS Initiate协商结果超过请求能力");
  }
  if (response.negotiatedVersionNumber > request.proposedVersionNumber ||
      !IsBitStringSubset(response.negotiatedParameterSupport,
                         request.proposedParameterSupport) ||
      !IsBitStringSubset(response.negotiatedServiceSupport,
                         request.proposedServiceSupport)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS Initiate协商支持集合超出请求能力");
  }
  const bool supportsNameList =
      response.negotiatedServiceSupport.size >= 1 &&
      (response.negotiatedServiceSupport.bytes[0] & kMmsSupportGetNameList) != 0;
  const bool supportsIdentify =
      response.negotiatedServiceSupport.size >= 1 &&
      (response.negotiatedServiceSupport.bytes[0] & kMmsSupportIdentify) != 0;
  const bool supportsVariableAttributes =
      response.negotiatedServiceSupport.size >= 1 &&
      (response.negotiatedServiceSupport.bytes[0] &
       kMmsSupportGetVariableAccessAttributes) != 0;
  const bool supportsRead =
      response.negotiatedServiceSupport.size >= 1 &&
      (response.negotiatedServiceSupport.bytes[0] & kMmsSupportRead) != 0;
  const bool supportsWrite =
      response.negotiatedServiceSupport.size >= 1 &&
      (response.negotiatedServiceSupport.bytes[0] & kMmsSupportWrite) != 0;
  const bool supportsFileOpen =
      response.negotiatedServiceSupport.size >= 10 &&
      (response.negotiatedServiceSupport.bytes[9] & kMmsSupportFileOpen) != 0;
  const bool supportsFileRead =
      response.negotiatedServiceSupport.size >= 10 &&
      (response.negotiatedServiceSupport.bytes[9] & kMmsSupportFileRead) != 0;
  const bool supportsFileClose =
      response.negotiatedServiceSupport.size >= 10 &&
      (response.negotiatedServiceSupport.bytes[9] & kMmsSupportFileClose) != 0;
  const bool supportsFileDirectory =
      response.negotiatedServiceSupport.size >= 10 &&
      (response.negotiatedServiceSupport.bytes[9] &
       kMmsSupportFileDirectory) != 0;
  if (!supportsNameList || !supportsRead || !supportsVariableAttributes) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        "IEC61850 MMS服务端未协商在线目录所需的NameList、Read或变量属性服务");
  }
  if (PlanRequiresMmsWrite(plan_) && !supportsWrite) {
    LOG_WARNING("IEC61850 MMS计划要求Write但服务端未协商: 通道={}, RCB数={}, 数据属性数={}, 数据集数={}",
                static_cast<int>(channel.channel),
                plan_.ied.report_controls_size(),
                plan_.ied.data_attributes_size(), plan_.ied.data_sets_size());
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS服务端未协商RCB或控制所需的Write服务");
  }
  const bool supportsNamedVariableListAttributes =
      response.negotiatedServiceSupport.size >= 2 &&
      (response.negotiatedServiceSupport.bytes[1] &
       kMmsSupportGetNamedVariableListAttributes) != 0;
  status = ReadOnlineDirectory(channel, stopToken, supportsRead,
                               supportsVariableAttributes,
                               supportsNamedVariableListAttributes,
                               supportsWrite);
  if (!status.ok()) {
    return status;
  }
  if (supportsIdentify) {
    status = ReadIdentify(channel, stopToken, &channel.nextInvokeId);
    if (!status.ok()) {
      return status;
    }
  } else {
    LOG_WARNING("IEC61850 MMS服务端未协商Identify，跳过厂商型号读取: 通道={}",
                static_cast<int>(channel.channel));
  }
  {
    std::lock_guard controlLock(channel.controlMutex);
    channel.supportsWrite = supportsWrite;
    channel.supportsFileDirectory = supportsFileDirectory;
    channel.supportsFileTransfer =
        supportsFileOpen && supportsFileRead && supportsFileClose;
  }
  LOG_INFO("IEC61850 MMS Initiate协商完成: 通道={}, Write={}, FileDirectory={}, FileTransfer={}",
           static_cast<int>(channel.channel), supportsWrite ? "支持" : "不支持",
           supportsFileDirectory ? "支持" : "不支持",
           supportsFileOpen && supportsFileRead && supportsFileClose ? "支持"
                                                                       : "不支持");
  return grpc::Status::OK;
}

grpc::Status MmsSessionWorker::ReadNameList(
    Channel& channel, MmsObjectClass objectClass, MmsObjectScope scope,
    std::stop_token stopToken, std::uint32_t* nextInvokeId,
    std::vector<std::string>* identifiers,
    const std::function<void(std::span<const std::uint8_t>)>&
        onUnconfirmed) {
  if (nextInvokeId == nullptr || identifiers == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS目录分页输出参数为空");
  }
  identifiers->clear();
  std::unordered_set<std::string> seenIdentifiers;
  std::optional<std::string> continueAfter;
  std::string previousLastIdentifier;
  std::size_t pageCount = 0;
  for (;;) {
    if (stopToken.stop_requested()) {
      return grpc::Status(grpc::StatusCode::CANCELLED,
                          "IEC61850 MMS在线目录读取已取消");
    }
    if (++pageCount > kMmsNameListPageLimit) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "IEC61850 MMS目录分页超过下位机上限");
    }
    if (*nextInvokeId == 0) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "IEC61850 MMS invokeID耗尽");
    }
    MmsGetNameListRequest request;
    request.objectClass = objectClass;
    request.scope = scope;
    request.continueAfter = continueAfter;
    std::array<std::uint8_t, kMmsPduBufferSize> requestBuffer{};
    std::size_t requestSize = 0;
    auto status = EncodeMmsGetNameListRequest(
        *nextInvokeId, request, requestBuffer, &requestSize);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> response;
    status = ExchangeMmsConfirmedRequest(
        *channel.transport,
        std::span<const std::uint8_t>(requestBuffer.data(), requestSize),
        &response, onUnconfirmed);
    if (!status.ok()) {
      return status;
    }
    MmsGetNameListResponse decoded;
    status = DecodeMmsGetNameListResponse(response, *nextInvokeId, &decoded);
    if (!status.ok()) {
      return status;
    }
    if (decoded.identifiers.size() >
        kMmsNameListEntryLimit -
            std::min(kMmsNameListEntryLimit, identifiers->size())) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "IEC61850 MMS NameList条目超过下位机上限");
    }
    for (const auto& identifier : decoded.identifiers) {
      if (!seenIdentifiers.emplace(identifier).second) {
        return grpc::Status(grpc::StatusCode::DATA_LOSS,
                            "IEC61850 MMS NameList包含重复Identifier");
      }
      identifiers->emplace_back(identifier);
    }
    LOG_INFO("IEC61850 MMS读取NameList: 通道={}, 对象类={}, 本页数量={}, 累计数量={}, 是否还有后续={}",
             static_cast<int>(channel.channel),
             static_cast<int>(objectClass), decoded.identifiers.size(),
             identifiers->size(), decoded.moreFollows ? "是" : "否");
    if (decoded.identifiers.empty() && decoded.moreFollows) {
      return grpc::Status(grpc::StatusCode::DATA_LOSS,
                          "IEC61850 MMS NameList返回空分页");
    }
    if (!decoded.moreFollows) {
      return grpc::Status::OK;
    }
    const auto& lastIdentifier = decoded.identifiers.back();
    if (lastIdentifier.empty() || lastIdentifier == previousLastIdentifier) {
      return grpc::Status(grpc::StatusCode::DATA_LOSS,
                          "IEC61850 MMS NameList continueAfter未前进");
    }
    previousLastIdentifier = lastIdentifier;
    continueAfter = lastIdentifier;
    status = AdvanceInvokeId(nextInvokeId);
    if (!status.ok()) {
      return status;
    }
  }
}

grpc::Status MmsSessionWorker::ReadIdentify(Channel& channel,
                                            std::stop_token stopToken,
                                            std::uint32_t* nextInvokeId) {
  if (nextInvokeId == nullptr || *nextInvokeId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS Identify invokeID参数无效");
  }
  if (stopToken.stop_requested()) {
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS Identify读取已取消");
  }
  std::array<std::uint8_t, kMmsPduBufferSize> requestBuffer{};
  std::size_t requestSize = 0;
  auto status = EncodeMmsIdentifyRequest(*nextInvokeId, requestBuffer,
                                         &requestSize);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> response;
  status = ExchangeMmsConfirmedRequest(
      *channel.transport,
      std::span<const std::uint8_t>(requestBuffer.data(), requestSize),
      &response,
      [this, channelId = channel.channel](std::span<const std::uint8_t> pdu) {
        EnqueueUnconfirmed(channelId, pdu);
      });
  if (!status.ok()) {
    return status;
  }
  MmsIdentifyResponse identity;
  status = DecodeMmsIdentifyResponse(response, *nextInvokeId, &identity);
  if (!status.ok()) {
    LOG_WARNING("IEC61850 MMS Identify响应解码失败: 通道={}, 报文={}, 原因={}",
                static_cast<int>(channel.channel), HexDump(response),
                status.error_message());
    return status;
  }
  LOG_INFO("IEC61850 MMS Identify完成: 通道={}, 厂商={}, 型号={}, 版本={}",
           static_cast<int>(channel.channel), identity.vendorName,
           identity.modelName, identity.revision);
  return AdvanceInvokeId(nextInvokeId);
}

grpc::Status MmsSessionWorker::ReadOnlineDirectory(
    Channel& channel, std::stop_token stopToken,
    bool supportsRead,
    bool supportsVariableAttributes,
    bool supportsNamedVariableListAttributes, bool supportsWrite) {
  if (!supportsRead) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS服务端未协商Read服务");
  }
  if (!supportsVariableAttributes) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        "IEC61850 MMS服务端未协商GetVariableAccessAttributes");
  }
  if (plan_.ied.data_sets_size() != 0 &&
      !supportsNamedVariableListAttributes) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        "IEC61850 MMS服务端未协商GetNamedVariableListAttributes");
  }
  if (PlanRequiresMmsWrite(plan_) && !supportsWrite) {
    LOG_WARNING("IEC61850 MMS在线目录计划要求Write但服务端未协商: 通道={}, RCB数={}, 数据属性数={}, 数据集数={}",
                static_cast<int>(channel.channel),
                plan_.ied.report_controls_size(),
                plan_.ied.data_attributes_size(), plan_.ied.data_sets_size());
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS在线目录需要Write服务才能配置RCB或控制");
  }
  std::uint32_t invokeId = 1;
  const auto enqueueUnconfirmed =
      [this, channelId = channel.channel](
          std::span<const std::uint8_t> pdu) {
        EnqueueUnconfirmed(channelId, pdu);
      };
  std::vector<std::string> domains;
  MmsObjectScope vmdScope;
  vmdScope.type = MmsObjectScopeType::VMD_SPECIFIC;
  auto status = ReadNameList(channel, MmsObjectClass::DOMAIN, vmdScope,
                             stopToken, &invokeId, &domains,
                             enqueueUnconfirmed);
  if (!status.ok()) {
    return status;
  }
  std::unordered_set<std::string> domainSet(domains.begin(), domains.end());
  if (domainSet.size() != domains.size()) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "IEC61850 MMS服务端Domain目录包含重复名称");
  }
  for (const auto& domain : domains) {
    if (domain.empty()) {
      return grpc::Status(grpc::StatusCode::DATA_LOSS,
                          "IEC61850 MMS服务端返回空Domain名称");
    }
  }

  struct DomainNames {
    std::vector<std::string> variables;
    std::vector<std::string> variableLists;
  };
  std::unordered_map<std::string, DomainNames> names;
  const auto hasDataSets = plan_.ied.data_sets_size() != 0;
  for (const auto& domain : domains) {
    MmsObjectScope scope;
    scope.type = MmsObjectScopeType::DOMAIN_SPECIFIC;
    scope.domain = domain;
    auto& domainNames = names[domain];
    status = ReadNameList(channel, MmsObjectClass::NAMED_VARIABLE, scope,
                          stopToken, &invokeId, &domainNames.variables,
                          enqueueUnconfirmed);
    if (!status.ok()) {
      return status;
    }
    if (hasDataSets) {
      status = ReadNameList(channel, MmsObjectClass::NAMED_VARIABLE_LIST,
                            scope, stopToken, &invokeId,
                            &domainNames.variableLists, enqueueUnconfirmed);
      if (!status.ok()) {
        return status;
      }
    }
  }

  const auto contains = [](const std::vector<std::string>& values,
                           std::string_view expected) {
    return std::ranges::find(values, expected) != values.end();
  };
  const auto containsNode = [](const std::vector<std::string>& values,
                               std::string_view expected) {
    const std::string prefix = std::format("{}$", expected);
    return std::ranges::any_of(values, [&](const auto& value) {
      return value == expected || value.starts_with(prefix);
    });
  };

  MmsOnlineDirectory directory;
  directory.iedName = plan_.config.ied_name();
  directory.accessPoint = plan_.config.access_point();
  for (const auto& domain : domains) {
    const auto namesIt = names.find(domain);
    if (namesIt == names.end()) {
      continue;
    }
    for (const auto& identifier : namesIt->second.variables) {
      MmsObjectName object;
      object.type = MmsObjectNameType::DOMAIN_SPECIFIC;
      object.domain = domain;
      object.identifier = identifier;
      directory.namedVariables.emplace_back(std::move(object));
    }
  }
  directory.logicalNodeRefs.reserve(plan_.ied.logical_nodes_size());
  for (const auto& node : plan_.ied.logical_nodes()) {
    MmsObjectName objectName;
    status = ParseMmsDomainObjectReference(node.node_ref(), &objectName);
    if (!status.ok()) {
      return status;
    }
    const auto namesIt = names.find(objectName.domain);
    if (!domainSet.contains(objectName.domain) || namesIt == names.end() ||
        !containsNode(namesIt->second.variables, objectName.identifier)) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("IEC61850 MMS在线目录缺少逻辑节点: {}", node.node_ref()));
    }
    directory.logicalNodeRefs.emplace_back(node.node_ref());
  }

  directory.dataAttributes.reserve(plan_.ied.data_attributes_size());
  for (const auto& expected : plan_.ied.data_attributes()) {
    MmsObjectName objectName;
    status = ParseMmsDomainObjectReference(expected.data_ref(), &objectName);
    if (!status.ok()) {
      return status;
    }
    const auto namesIt = names.find(objectName.domain);
    if (!domainSet.contains(objectName.domain) || namesIt == names.end() ||
        !contains(namesIt->second.variables, objectName.identifier)) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("IEC61850 MMS在线目录缺少数据属性: {}#{}",
                      expected.data_ref(), static_cast<int>(expected.fc())));
    }
    std::array<std::uint8_t, kMmsPduBufferSize> requestBuffer{};
    std::size_t requestSize = 0;
    status = EncodeMmsGetVariableAccessAttributesRequest(
        invokeId, objectName, requestBuffer, &requestSize);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> response;
    status = ExchangeMmsConfirmedRequest(
        *channel.transport,
        std::span<const std::uint8_t>(requestBuffer.data(), requestSize),
        &response, enqueueUnconfirmed);
    if (!status.ok()) {
      return status;
    }
    MmsGetVariableAccessAttributesResponse attributes;
    status = DecodeMmsGetVariableAccessAttributesResponse(response, invokeId,
                                                           &attributes);
    if (!status.ok()) {
      return status;
    }
    MmsDirectoryDataAttribute actual;
    actual.dataRef = expected.data_ref();
    actual.fc = expected.fc();
    actual.typeSpecification = std::move(attributes.typeSpecification);
    if (expected.fc() == IEC61850Proto::FUNCTIONAL_CONSTRAINT_CF &&
        NeedsOnlineControlValue(expected.data_ref())) {
      MmsReadRequest valueRequest;
      valueRequest.variables.emplace_back(objectName);
      std::array<std::uint8_t, kMmsPduBufferSize> valueRequestBuffer{};
      std::size_t valueRequestSize = 0;
      status = EncodeMmsReadRequest(invokeId, valueRequest, valueRequestBuffer,
                                    &valueRequestSize);
      if (!status.ok()) {
        return status;
      }
      std::vector<std::uint8_t> valueResponse;
      status = ExchangeMmsConfirmedRequest(
          *channel.transport,
          std::span<const std::uint8_t>(valueRequestBuffer.data(),
                                         valueRequestSize),
          &valueResponse, enqueueUnconfirmed);
      if (!status.ok()) {
        return status;
      }
      MmsReadResponse decodedValue;
      status = DecodeMmsReadResponse(valueResponse, invokeId, &decodedValue);
      if (!status.ok()) {
        return status;
      }
      if (decodedValue.items.size() != 1 ||
          !decodedValue.items.front().success ||
          decodedValue.items.front().encodedData.empty()) {
        return grpc::Status(
            grpc::StatusCode::DATA_LOSS,
            std::format("IEC61850 MMS在线控制参数Read失败: {}",
                        expected.data_ref()));
      }
      actual.encodedValue =
          std::move(decodedValue.items.front().encodedData);
      LOG_DEBUG("IEC61850 MMS读取在线控制参数: 通道={}, 参数={}",
                static_cast<int>(channel.channel), expected.data_ref());
    }
    directory.dataAttributes.emplace_back(std::move(actual));
    status = AdvanceInvokeId(&invokeId);
    if (!status.ok()) {
      return status;
    }
  }

  directory.dataSets.reserve(plan_.ied.data_sets_size());
  for (const auto& expected : plan_.ied.data_sets()) {
    MmsObjectName objectName;
    status = ParseMmsDomainObjectReference(expected.data_set_ref(),
                                            &objectName);
    if (!status.ok()) {
      return status;
    }
    const auto namesIt = names.find(objectName.domain);
    if (!domainSet.contains(objectName.domain) || namesIt == names.end() ||
        !contains(namesIt->second.variableLists, objectName.identifier)) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("IEC61850 MMS在线目录缺少DataSet: {}",
                      expected.data_set_ref()));
    }
    std::array<std::uint8_t, kMmsPduBufferSize> requestBuffer{};
    std::size_t requestSize = 0;
    status = EncodeMmsGetNamedVariableListAttributesRequest(
        invokeId, objectName, requestBuffer, &requestSize);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> response;
    status = ExchangeMmsConfirmedRequest(
      *channel.transport,
        std::span<const std::uint8_t>(requestBuffer.data(), requestSize),
        &response, enqueueUnconfirmed);
    if (!status.ok()) {
      return status;
    }
    MmsGetNamedVariableListAttributesResponse attributes;
    status = DecodeMmsGetNamedVariableListAttributesResponse(
        response, invokeId, &attributes);
    if (!status.ok()) {
      return status;
    }
    MmsDirectoryDataSet actual;
    actual.dataSetRef = expected.data_set_ref();
    actual.members.reserve(attributes.variables.size());
    for (const auto& onlineVariable : attributes.variables) {
      bool matched = false;
      for (const auto& expectedMember : expected.members()) {
        MmsObjectName expectedObject;
        status = ParseMmsDomainObjectReference(expectedMember.data_ref(),
                                                &expectedObject);
        if (!status.ok()) {
          return status;
        }
        if (onlineVariable.type == expectedObject.type &&
            onlineVariable.domain == expectedObject.domain &&
            onlineVariable.identifier == expectedObject.identifier) {
          actual.members.push_back({expectedMember.data_ref(),
                                    expectedMember.fc(), std::nullopt});
          matched = true;
          break;
        }
      }
      if (!matched) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("IEC61850 MMS在线DataSet包含计划外成员: {}",
                        expected.data_set_ref()));
      }
    }
    directory.dataSets.emplace_back(std::move(actual));
    status = AdvanceInvokeId(&invokeId);
    if (!status.ok()) {
      return status;
    }
  }

  // 读取每个RCB根对象。RCB根对象的Read结果是一个Data.structure，
  // 由自研RCB解码器按BRCB/URCB结构核对标准属性。
  directory.reportControls.reserve(plan_.ied.report_controls_size());
  for (const auto& expected : plan_.ied.report_controls()) {
    MmsObjectName objectName;
    status = ParseMmsDomainObjectReference(expected.rcb_ref(), &objectName);
    if (!status.ok()) {
      return status;
    }
    const auto namesIt = names.find(objectName.domain);
    if (!domainSet.contains(objectName.domain) || namesIt == names.end()) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("IEC61850 MMS在线目录缺少ReportControl: {}",
                      expected.rcb_ref()));
    }

    if (!contains(namesIt->second.variables, objectName.identifier)) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("IEC61850 MMS在线目录缺少ReportControl根对象: {}",
                      expected.rcb_ref()));
    }
    MmsReadRequest request;
    request.variables.emplace_back(objectName);

    std::array<std::uint8_t, kMmsPduBufferSize> requestBuffer{};
    std::size_t requestSize = 0;
    status = EncodeMmsReadRequest(invokeId, request, requestBuffer,
                                  &requestSize);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> response;
    status = ExchangeMmsConfirmedRequest(
        *channel.transport,
        std::span<const std::uint8_t>(requestBuffer.data(), requestSize),
        &response, enqueueUnconfirmed);
    if (!status.ok()) {
      return status;
    }
    MmsReadResponse decoded;
    status = DecodeMmsReadResponse(response, invokeId, &decoded);
    if (!status.ok()) {
      return status;
    }
    if (decoded.items.size() != 1 || !decoded.items.front().success) {
      return grpc::Status(
          grpc::StatusCode::DATA_LOSS,
          std::format("IEC61850 MMS RCB根对象Read未返回单个成功Data: {}",
                      expected.rcb_ref()));
    }
    MmsDirectoryReportControl actual;
    status = DecodeMmsRcbData(decoded.items.front().encodedData,
                              expected.buffered(), &actual);
    if (!status.ok()) {
      return grpc::Status(
          status.error_code(),
          std::format("IEC61850 MMS RCB属性解码失败: {}，错误={}",
                      expected.rcb_ref(), status.error_message()));
    }
    actual.rcbRef = expected.rcb_ref();
    actual.buffered = expected.buffered();
    actual.maxInstances = expected.max_instances();
    directory.reportControls.emplace_back(std::move(actual));
    status = AdvanceInvokeId(&invokeId);
    if (!status.ok()) {
      return status;
    }
  }

  MmsControlModel controlModel;
  status = BuildMmsControlModel(plan_, directory, &controlModel);
  if (!status.ok()) {
    LOG_WARNING("IEC61850 MMS在线控制能力模型构建失败: 通道={}, 原因={}",
                static_cast<int>(channel.channel), status.error_message());
    return status;
  }
  {
    std::lock_guard controlLock(channel.controlMutex);
    channel.controlModel = std::make_shared<MmsControlModel>(
        std::move(controlModel));
  }

  auto contract = std::make_unique<MmsSessionContract>(plan_);
  const auto validationStage = MmsDirectoryValidationStage::COMPLETE;
  status = contract->ValidateOnlineDirectory(directory, validationStage);
  if (!status.ok()) {
    return status;
  }
  status = ConfigureReportControls(channel, stopToken, &invokeId, contract.get());
  if (!status.ok()) {
    return status;
  }
  channel.contract = std::move(contract);
  channel.nextInvokeId = invokeId;
  LOG_INFO("IEC61850 MMS在线目录核对完成: 通道={}, Domain数={}, 数据属性数={}, DataSet数={}, RCB属性数={}",
           static_cast<int>(channel.channel), domains.size(),
           directory.dataAttributes.size(), directory.dataSets.size(),
           directory.reportControls.size());
  return grpc::Status::OK;
}

grpc::Status MmsSessionWorker::ConfigureReportControls(
    Channel& channel, std::stop_token stopToken, std::uint32_t* nextInvokeId,
    MmsSessionContract* contract) {
  if (nextInvokeId == nullptr || contract == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS RCB配置参数为空");
  }
  if (plan_.ied.report_controls_size() == 0) {
    return grpc::Status::OK;
  }
  if (!TryClaimRcbConfiguration(channel.channel)) {
    LOG_INFO("IEC61850 MMS跳过备用通道RCB写入: 通道={}",
             static_cast<int>(channel.channel));
    return grpc::Status::OK;
  }
  // 当前会话已经取得配置权，丢弃建立过程中遗留的同通道接管请求，
  // 避免成功完成RCB配置后又被旧请求触发一次无谓重连。
  ConsumeRcbReconfigurationRequest(channel.channel);
  const auto fail = [this, &channel](grpc::Status value) {
    ReleaseRcbConfiguration(channel.channel);
    return value;
  };
  const auto enqueueUnconfirmed =
      [this, channelId = channel.channel](
          std::span<const std::uint8_t> pdu) {
        EnqueueUnconfirmed(channelId, pdu);
      };
  const auto requests = contract->BuildRcbActivationRequests();
  constexpr MmsRcbWritePhase phases[] = {
      MmsRcbWritePhase::DISABLE, MmsRcbWritePhase::CONFIGURE,
      MmsRcbWritePhase::ENABLE};
  for (const auto& activation : requests) {
    for (const auto phase : phases) {
      if (stopToken.stop_requested()) {
        return fail(grpc::Status(grpc::StatusCode::CANCELLED,
                                 "IEC61850 MMS RCB配置已取消"));
      }
      if (*nextInvokeId == 0) {
        return fail(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                 "IEC61850 MMS RCB配置invokeID耗尽"));
      }
      MmsWriteRequest request;
      auto status = BuildMmsRcbWriteRequest(activation, phase, &request);
      if (!status.ok()) {
        return fail(status);
      }
      std::array<std::uint8_t, kMmsPduBufferSize> requestBuffer{};
      std::size_t requestSize = 0;
      status = EncodeMmsWriteRequest(*nextInvokeId, request, requestBuffer,
                                     &requestSize);
      if (!status.ok()) {
        return fail(status);
      }
      std::vector<std::uint8_t> response;
      status = ExchangeMmsConfirmedRequest(
          *channel.transport,
          std::span<const std::uint8_t>(requestBuffer.data(), requestSize),
          &response, enqueueUnconfirmed);
      if (!status.ok()) {
        return fail(status);
      }
      MmsWriteResponse decoded;
      status = DecodeMmsWriteResponse(response, *nextInvokeId, &decoded);
      if (!status.ok()) {
        return fail(status);
      }
      if (decoded.items.size() != request.items.size()) {
        return fail(grpc::Status(
            grpc::StatusCode::DATA_LOSS,
            std::format("IEC61850 MMS RCB Write响应数量不匹配: {}",
                        activation.rcbRef)));
      }
      for (const auto& item : decoded.items) {
        if (!item.success) {
          return fail(grpc::Status(
              grpc::StatusCode::FAILED_PRECONDITION,
              std::format("IEC61850 MMS RCB Write被服务端拒绝: {}, 错误码={}",
                          activation.rcbRef, item.failureCode)));
        }
      }
      LOG_INFO("IEC61850 MMS RCB写入成功: 通道={}, RCB={}, 阶段={}, 项数={}",
               static_cast<int>(channel.channel), activation.rcbRef,
               static_cast<int>(phase), request.items.size());
      status = AdvanceInvokeId(nextInvokeId);
      if (!status.ok()) {
        return fail(status);
      }
      if (phase == MmsRcbWritePhase::ENABLE) {
        status = contract->MarkRcbEnabled(activation.rcbRef,
                                          activation.configRevision);
        if (!status.ok()) {
          return fail(status);
        }
      }
    }
  }

  // GI必须在所有RCB启用确认后逐个独立写入，不能混入配置字段Write。
  for (const auto& activation : requests) {
    if (!activation.generalInterrogation) {
      continue;
    }
    if (stopToken.stop_requested()) {
      return fail(grpc::Status(grpc::StatusCode::CANCELLED,
                               "IEC61850 MMS GI请求已取消"));
    }
    if (*nextInvokeId == 0) {
      return fail(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                               "IEC61850 MMS GI请求invokeID耗尽"));
    }
    MmsWriteRequest request;
    auto status = BuildMmsRcbGeneralInterrogationRequest(activation, &request);
    if (!status.ok()) {
      return fail(status);
    }
    std::array<std::uint8_t, kMmsPduBufferSize> requestBuffer{};
    std::size_t requestSize = 0;
    status = EncodeMmsWriteRequest(*nextInvokeId, request, requestBuffer,
                                   &requestSize);
    if (!status.ok()) {
      return fail(status);
    }
    std::vector<std::uint8_t> response;
    status = ExchangeMmsConfirmedRequest(
        *channel.transport,
        std::span<const std::uint8_t>(requestBuffer.data(), requestSize),
        &response, enqueueUnconfirmed);
    if (!status.ok()) {
      return fail(status);
    }
    MmsWriteResponse decoded;
    status = DecodeMmsWriteResponse(response, *nextInvokeId, &decoded);
    if (!status.ok()) {
      return fail(status);
    }
    if (decoded.items.size() != request.items.size()) {
      return fail(grpc::Status(
          grpc::StatusCode::DATA_LOSS,
          std::format("IEC61850 MMS GI Write响应数量不匹配: {}",
                      activation.rcbRef)));
    }
    for (const auto& item : decoded.items) {
      if (!item.success) {
        return fail(grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("IEC61850 MMS GI Write被服务端拒绝: {}, 错误码={}",
                        activation.rcbRef, item.failureCode)));
      }
    }
    LOG_INFO("IEC61850 MMS GI请求写入成功: 通道={}, RCB={}",
             static_cast<int>(channel.channel), activation.rcbRef);
    status = contract->MarkGeneralInterrogationRequested(activation.rcbRef);
    if (!status.ok()) {
      return fail(status);
    }
    status = AdvanceInvokeId(nextInvokeId);
    if (!status.ok()) {
      return fail(status);
    }
  }
  ConsumeRcbReconfigurationRequest(channel.channel);
  return grpc::Status::OK;
}

void MmsSessionWorker::EnqueueUnconfirmed(
    IEC61850Proto::NetworkChannel channel,
    std::span<const std::uint8_t> pdu) {
  if (pdu.empty()) {
    return;
  }
  if (pdu.size() > kPendingUnconfirmedBytesLimit) {
    LOG_WARNING("IEC61850 MMS未确认报告超过暂存字节上限，丢弃报文");
    return;
  }
  std::lock_guard lock(sharedState_->mutex);
  while (!sharedState_->pendingUnconfirmed.empty() &&
         (sharedState_->pendingUnconfirmed.size() >= kPendingUnconfirmedPduLimit ||
          sharedState_->pendingUnconfirmedBytes + pdu.size() >
              kPendingUnconfirmedBytesLimit)) {
    sharedState_->pendingUnconfirmedBytes -=
        sharedState_->pendingUnconfirmed.front().pdu.size();
    sharedState_->pendingUnconfirmed.pop_front();
    LOG_WARNING("IEC61850 MMS未确认报告队列已满，丢弃最早报文");
  }
  SharedState::PendingUnconfirmed pending;
  pending.channel = channel;
  pending.pdu.assign(pdu.begin(), pdu.end());
  sharedState_->pendingUnconfirmedBytes += pending.pdu.size();
  sharedState_->pendingUnconfirmed.emplace_back(std::move(pending));
}

void MmsSessionWorker::DrainPendingUnconfirmed(
    Channel& channel, const std::vector<MmsReportDecodePlan>& reportPlans) {
  std::deque<std::vector<std::uint8_t>> pending;
  {
    std::lock_guard lock(sharedState_->mutex);
    for (auto it = sharedState_->pendingUnconfirmed.begin();
         it != sharedState_->pendingUnconfirmed.end();) {
      if (it->channel == channel.channel) {
        sharedState_->pendingUnconfirmedBytes -= it->pdu.size();
        pending.emplace_back(std::move(it->pdu));
        it = sharedState_->pendingUnconfirmed.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const auto& pdu : pending) {
    LOG_DEBUG("IEC61850 MMS处理暂存InformationReport: 通道={}, 报文={}",
              static_cast<int>(channel.channel), HexDump(pdu));
    ProcessMmsInformationReport(channel, pdu, reportPlans);
  }
}

bool MmsSessionWorker::TryClaimRcbConfiguration(
    IEC61850Proto::NetworkChannel channel) {
  std::lock_guard lock(sharedState_->mutex);
  if (!sharedState_->rcbConfigurationChannel.has_value()) {
    // A优先：较低编号通道仍在建链或已连接时，备用通道只完成目录核对。
    // 较低通道已经失败/断开后，当前通道可以接管RCB写入和GI。
    for (const auto& candidate : sharedState_->channels) {
      if (candidate.channel >= channel ||
          (candidate.state != IEC61850Proto::CHANNEL_STATE_CONNECTING &&
           candidate.state != IEC61850Proto::CHANNEL_STATE_CONNECTED)) {
        continue;
      }
      return false;
    }
    sharedState_->rcbConfigurationChannel = channel;
    return true;
  }
  return *sharedState_->rcbConfigurationChannel == channel;
}

void MmsSessionWorker::ReleaseRcbConfiguration(
    IEC61850Proto::NetworkChannel channel) {
  std::lock_guard lock(sharedState_->mutex);
  if (sharedState_->rcbConfigurationChannel == channel) {
    sharedState_->rcbConfigurationChannel.reset();
  }
}

bool MmsSessionWorker::ConsumeRcbReconfigurationRequest(
    IEC61850Proto::NetworkChannel channel) {
  std::lock_guard lock(sharedState_->mutex);
  const auto it = sharedState_->rcbReconfigurationRequests.find(
      static_cast<int>(channel));
  if (it == sharedState_->rcbReconfigurationRequests.end()) {
    return false;
  }
  sharedState_->rcbReconfigurationRequests.erase(it);
  return true;
}

void MmsSessionWorker::ProcessMmsInformationReport(
    Channel& channel, std::span<const std::uint8_t> mmsPdu,
    const std::vector<MmsReportDecodePlan>& reportPlans) {
  if (mmsPdu.empty() || mmsPdu.front() != 0xa3) {
    return;
  }
  {
    std::lock_guard stateLock(sharedState_->mutex);
    if (sharedState_->activeChannel !=
            IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED &&
        sharedState_->activeChannel != channel.channel) {
      LOG_DEBUG("IEC61850 MMS丢弃非活动通道报告: 通道={}, 活动通道={}, 报文={}",
                static_cast<int>(channel.channel),
                static_cast<int>(sharedState_->activeChannel), HexDump(mmsPdu));
      return;
    }
  }
  std::optional<MmsReportSegment> decodedSegment;
  grpc::Status decodeStatus(
      grpc::StatusCode::FAILED_PRECONDITION,
      "IEC61850 MMS未找到匹配的ReportControl");
  for (const auto& reportPlan : reportPlans) {
    MmsReportSegment segment;
    const auto candidateStatus =
        DecodeMmsInformationReport(mmsPdu, reportPlan, &segment);
    if (candidateStatus.ok()) {
      decodedSegment.emplace(std::move(segment));
      decodeStatus = grpc::Status::OK;
      break;
    }
    decodeStatus = candidateStatus;
  }
  if (!decodedSegment.has_value()) {
    LOG_WARNING(
        "IEC61850 MMS InformationReport解码失败: 通道={}, 原因={}, 报文={}",
        static_cast<int>(channel.channel), decodeStatus.error_message(),
        HexDump(mmsPdu));
    return;
  }

  const auto completedReport =
      channel.reportAssembler.Push(std::move(*decodedSegment), NowMs());
  if (!completedReport.has_value()) {
    return;
  }
  if (channel.contract == nullptr) {
    LOG_WARNING("IEC61850 MMS报告缺少会话契约，已丢弃: 通道={}, RCB={}",
                static_cast<int>(channel.channel), completedReport->reportRef);
    return;
  }
  bool acceptedGeneralInterrogation = false;
  if (completedReport->generalInterrogation) {
    const auto status =
        channel.contract->MarkGeneralInterrogationComplete(*completedReport);
    if (!status.ok()) {
      LOG_WARNING("IEC61850 MMS GI报告完成确认失败，已丢弃: 通道={}, RCB={}, 原因={}",
                  static_cast<int>(channel.channel),
                  completedReport->reportRef, status.error_message());
      return;
    }
    acceptedGeneralInterrogation = true;
  }
  const bool contractReady = channel.contract->Ready();
  if (!contractReady && !acceptedGeneralInterrogation) {
    LOG_DEBUG("IEC61850 MMS会话未READY，丢弃普通报告: 通道={}, RCB={}",
              static_cast<int>(channel.channel), completedReport->reportRef);
    return;
  }
  if (contractReady && SetChannelReady(channel.channel, true)) {
    LOG_INFO("IEC61850 MMS会话已完成目录、RCB和GI准备: 通道={}",
             static_cast<int>(channel.channel));
    PublishState(MmsConnectionEventType::STATE_SNAPSHOT,
                 ProtocolSessionState::READY, channels_.size(), {});
  }
  if (!callbacks_.onMmsReport) {
    return;
  }
  try {
    LOG_DEBUG(
        "IEC61850 MMS完成报告分段合并: 通道={}, RCB={}, DataSet={}, 序号={}, 点数={}",
        static_cast<int>(channel.channel), completedReport->reportRef,
        completedReport->dataSetRef, completedReport->sequenceNumber,
        completedReport->values.size());
    callbacks_.onMmsReport(std::move(*completedReport));
  } catch (const std::exception& exception) {
    LOG_ERROR("IEC61850 MMS报告回调发生异常: {}", exception.what());
  } catch (...) {
    LOG_ERROR("IEC61850 MMS报告回调发生未知异常");
  }
}

void MmsSessionWorker::Run(std::size_t index, std::stop_token stopToken) {
  auto& channel = *channels_[index];
  const auto reportPlans = BuildMmsReportDecodePlans(plan_);
  bool firstAttempt = true;
  while (!stopToken.stop_requested()) {
    if (!firstAttempt) {
      PublishState(MmsConnectionEventType::RECONNECT_ATTEMPT,
                   ProtocolSessionState::CONNECTING, index, {});
    }
    UpdateChannel(index, IEC61850Proto::CHANNEL_STATE_CONNECTING, {});
    PublishState(MmsConnectionEventType::STATE_SNAPSHOT,
                 ProtocolSessionState::CONNECTING, index, {});
    channel.contract.reset();
    channel.reportAssembler.Clear();
    {
      std::lock_guard controlLock(channel.controlMutex);
      channel.controlModel.reset();
      channel.supportsWrite = false;
      channel.supportsFileDirectory = false;
      channel.supportsFileTransfer = false;
    }
    channel.sboState.Clear();
    auto status = Establish(channel, stopToken);
    if (status.ok()) {
      UpdateChannel(index, IEC61850Proto::CHANNEL_STATE_CONNECTED, {});
      PublishState(MmsConnectionEventType::STATE_SNAPSHOT,
                   ProtocolSessionState::CONNECTED, index, {});
      if (channel.contract != nullptr && channel.contract->Ready() &&
          SetChannelReady(channel.channel, true)) {
        LOG_INFO("IEC61850 MMS会话无需GI即可进入READY: 通道={}",
                 static_cast<int>(channel.channel));
        PublishState(MmsConnectionEventType::STATE_SNAPSHOT,
                     ProtocolSessionState::READY, channels_.size(), {});
      }

      DrainPendingUnconfirmed(channel, reportPlans);
      for (;;) {
        if (stopToken.stop_requested()) {
          break;
        }
        ProcessControlRequests(channel, stopToken);
        DrainPendingUnconfirmed(channel, reportPlans);
        if (stopToken.stop_requested()) {
          break;
        }
        if (ConsumeRcbReconfigurationRequest(channel.channel)) {
          LOG_INFO("IEC61850 MMS收到RCB接管请求，准备重建会话: 通道={}",
                   static_cast<int>(channel.channel));
          status = grpc::Status(
              grpc::StatusCode::UNAVAILABLE,
              "IEC61850 MMS活动通道已切换，备用通道重新接管RCB");
          break;
        }
        std::vector<std::uint8_t> payload;
        status = channel.transport->Receive(&payload, kMmsRunReceivePollMs);
        if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
          if (ConsumeRcbReconfigurationRequest(channel.channel)) {
            LOG_INFO("IEC61850 MMS收到RCB接管请求，准备重建会话: 通道={}",
                     static_cast<int>(channel.channel));
            status = grpc::Status(
                grpc::StatusCode::UNAVAILABLE,
                "IEC61850 MMS活动通道已切换，备用通道重新接管RCB");
            break;
          }
          continue;
        }
        if (!status.ok()) {
          break;
        }
        IsoSessionPduView pdu;
        status = DecodeIsoSessionPdu(payload, &pdu);
        if (!status.ok()) {
          break;
        }
        if (pdu.type == IsoSessionPduType::FINISH ||
            pdu.type == IsoSessionPduType::DISCONNECT ||
            pdu.type == IsoSessionPduType::ABORT) {
          status = grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "IEC61850 MMS服务端关闭Session会话");
          break;
        }
        if (pdu.type != IsoSessionPduType::DATA) {
          continue;
        }
        std::span<const std::uint8_t> mmsPdu;
        status = DecodeMmsPresentationData(pdu.userData, &mmsPdu);
        if (!status.ok()) {
          break;
        }
        LOG_DEBUG("IEC61850 MMS收到已建立会话PDU: 通道={}, 长度={}, 报文={}",
                  static_cast<int>(channel.channel), mmsPdu.size(),
                  HexDump(mmsPdu));
        ProcessMmsInformationReport(channel, mmsPdu, reportPlans);
      }
    }
    if (!status.ok() && !stopToken.stop_requested()) {
      LOG_WARNING("IEC61850 MMS会话建立失败: 通道={}, 错误码={}, 原因={}",
                  static_cast<int>(channel.channel),
                  static_cast<int>(status.error_code()),
                  status.error_message());
    }
    channel.transport->Close();
    CancelControlRequests(
        channel,
        grpc::Status(grpc::StatusCode::UNAVAILABLE,
                     "IEC61850 MMS活动通道已断开"));
    channel.contract.reset();
    channel.reportAssembler.Clear();
    {
      std::lock_guard controlLock(channel.controlMutex);
      channel.controlModel.reset();
    }
    channel.sboState.Clear();
    {
      std::lock_guard stateLock(sharedState_->mutex);
      for (auto it = sharedState_->pendingUnconfirmed.begin();
           it != sharedState_->pendingUnconfirmed.end();) {
        if (it->channel == channel.channel) {
          sharedState_->pendingUnconfirmedBytes -= it->pdu.size();
          it = sharedState_->pendingUnconfirmed.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (stopToken.stop_requested()) {
      break;
    }
    const auto reason = status.ok() ? "IEC61850 MMS会话意外结束"
                                    : status.error_message();
    UpdateChannel(index, IEC61850Proto::CHANNEL_STATE_ERROR, reason);
    PublishState(MmsConnectionEventType::STATE_SNAPSHOT,
                 ProtocolSessionState::DEGRADED, index, reason);
    firstAttempt = false;
    for (std::uint32_t elapsed = 0;
         elapsed < kMmsRetryDelayMs && !stopToken.stop_requested();
         elapsed += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (channel.transport != nullptr) {
    channel.transport->Close();
  }
  UpdateChannel(index, IEC61850Proto::CHANNEL_STATE_DISCONNECTED, {});
  PublishState(MmsConnectionEventType::STATE_SNAPSHOT,
               ProtocolSessionState::DISCONNECTED, index, {});
}

void MmsSessionWorker::UpdateChannel(std::size_t index,
                                     IEC61850Proto::ChannelState state,
                                     std::string error) {
  std::lock_guard lock(sharedState_->mutex);
  if (index >= sharedState_->channels.size()) {
    return;
  }
  const auto channel = sharedState_->channels[index].channel;
  const bool channelUnavailable =
      state == IEC61850Proto::CHANNEL_STATE_ERROR ||
      state == IEC61850Proto::CHANNEL_STATE_DISCONNECTED;
  const bool lostActiveChannel =
      channelUnavailable &&
      sharedState_->activeChannel == channel;
  const bool lostConfigurationChannel =
      channelUnavailable &&
      sharedState_->rcbConfigurationChannel == channel;
  sharedState_->channels[index].state = state;
  sharedState_->channels[index].error = std::move(error);
  if (state != IEC61850Proto::CHANNEL_STATE_CONNECTED) {
    sharedState_->readyChannels.erase(static_cast<int>(channel));
    for (const auto& candidate : channels_) {
      if (candidate->channel != channel) {
        continue;
      }
      std::lock_guard controlLock(candidate->controlMutex);
      candidate->acceptControlRequests = false;
      break;
    }
  }
  if (state != IEC61850Proto::CHANNEL_STATE_CONNECTED &&
      sharedState_->rcbConfigurationChannel ==
          sharedState_->channels[index].channel) {
    sharedState_->rcbConfigurationChannel.reset();
  }
  const auto isConnected = [&](IEC61850Proto::NetworkChannel candidate) {
    return std::ranges::any_of(sharedState_->channels, [&](const auto& status) {
      return status.channel == candidate && IsChannelConnected(status);
    });
  };
  if (sharedState_->rcbConfigurationChannel.has_value() &&
      isConnected(*sharedState_->rcbConfigurationChannel)) {
    sharedState_->activeChannel = *sharedState_->rcbConfigurationChannel;
  } else if (!isConnected(sharedState_->activeChannel)) {
    sharedState_->activeChannel =
        IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
    for (const auto& status : sharedState_->channels) {
      if (IsChannelConnected(status) &&
          (sharedState_->activeChannel ==
               IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED ||
           status.channel < sharedState_->activeChannel)) {
        sharedState_->activeChannel = status.channel;
      }
    }
  }
  if ((lostActiveChannel || lostConfigurationChannel ||
       (channelUnavailable &&
        !sharedState_->rcbConfigurationChannel.has_value())) &&
      plan_.ied.report_controls_size() != 0) {
    for (const auto& status : sharedState_->channels) {
      if (status.channel != channel &&
          status.state == IEC61850Proto::CHANNEL_STATE_CONNECTED) {
        sharedState_->rcbReconfigurationRequests.emplace(
            static_cast<int>(status.channel));
      }
    }
  }
  if (state == IEC61850Proto::CHANNEL_STATE_CONNECTED &&
      plan_.ied.report_controls_size() != 0 &&
      ShouldScheduleRcbReconfigurationAfterConnect(
          channel, sharedState_->channels,
          sharedState_->rcbConfigurationChannel)) {
    sharedState_->rcbReconfigurationRequests.emplace(
        static_cast<int>(channel));
    LOG_INFO("IEC61850 MMS备用通道完成建链后安排RCB接管: 通道={}",
             static_cast<int>(channel));
  }
}

bool MmsSessionWorker::SetChannelReady(
    IEC61850Proto::NetworkChannel channel, bool ready) {
  std::lock_guard lock(sharedState_->mutex);
  bool changed = false;
  if (ready) {
    changed = sharedState_->readyChannels
                  .emplace(static_cast<int>(channel))
                  .second;
  } else {
    changed = sharedState_->readyChannels.erase(static_cast<int>(channel)) != 0;
  }
  for (const auto& candidate : channels_) {
    if (candidate->channel != channel) {
      continue;
    }
    std::lock_guard controlLock(candidate->controlMutex);
    candidate->acceptControlRequests = ready;
    break;
  }
  return changed;
}

void MmsSessionWorker::PublishState(MmsConnectionEventType type,
                                    ProtocolSessionState state,
                                    std::size_t reconnectIndex,
                                    std::string error) {
  if (!callbacks_.onMmsConnection) {
    return;
  }
  MmsConnectionEvent event;
  event.type = type;
  event.state = state;
  event.timestampMs = NowMs();
  event.error = std::move(error);
  if (reconnectIndex < channels_.size()) {
    event.reconnectChannel = channels_[reconnectIndex]->channel;
  }
  {
    std::lock_guard lock(sharedState_->mutex);
    event.channels = sharedState_->channels;
    event.activeChannel = sharedState_->activeChannel;
    const bool activeReady =
        event.activeChannel != IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED &&
        sharedState_->readyChannels.contains(
            static_cast<int>(event.activeChannel)) &&
        std::ranges::any_of(event.channels, [&](const auto& status) {
          return status.channel == event.activeChannel &&
                 IsChannelConnected(status);
        });
    const bool hasConnected = std::any_of(
        event.channels.begin(), event.channels.end(), IsChannelConnected);
    if (hasConnected) {
      // 状态快照代表整个IED；备用通道的断线、重连或错误不能覆盖仍健康的活动通道。
      event.state = activeReady ? ProtocolSessionState::READY
                                : ProtocolSessionState::CONNECTED;
      event.error.clear();
    } else if (state == ProtocolSessionState::CONNECTED ||
               state == ProtocolSessionState::READY) {
      event.state = ProtocolSessionState::DEGRADED;
      event.activeChannel = IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
      event.error = "IEC61850 MMS没有可用活动通道";
    } else {
      event.activeChannel = IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
    }
  }
  try {
    callbacks_.onMmsConnection(std::move(event));
  } catch (const std::exception& exception) {
    LOG_ERROR("IEC61850 MMS连接状态回调发生异常: {}", exception.what());
  } catch (...) {
    LOG_ERROR("IEC61850 MMS连接状态回调发生未知异常");
  }
}

}  // namespace IEC61850
