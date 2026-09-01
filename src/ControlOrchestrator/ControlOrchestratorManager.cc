#include "ControlOrchestratorManager.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <string_view>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "Logger.h"

namespace ControlOrchestrator {
namespace {
grpc::Status invalid(std::string reason) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(reason));
}

grpc::Status notFound(const std::string &name) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND,
                      std::format("未找到命令编排: {}", name));
}

constexpr std::string_view kWorkflowPathMarker = "\x1eCO_PATH=";
constexpr auto kInterruptCheckInterval = std::chrono::milliseconds(10);
constexpr size_t kMaxWorkflowPath = 32;

struct RequestIdParts {
  std::string base;
  std::vector<std::string> workflowPath;
};

RequestIdParts splitRequestId(const std::string &requestId) {
  RequestIdParts parts{.base = requestId};
  const auto marker = requestId.rfind(kWorkflowPathMarker);
  if (marker == std::string::npos) {
    return parts;
  }
  parts.base = requestId.substr(0, marker);
  size_t cursor = marker + kWorkflowPathMarker.size();
  while (cursor < requestId.size()) {
    const auto separator = requestId.find(':', cursor);
    if (separator == std::string::npos || separator == cursor) {
      parts.workflowPath.clear();
      parts.base = requestId;
      return parts;
    }
    uint64_t length = 0;
    const auto *begin = requestId.data() + cursor;
    const auto *end = requestId.data() + separator;
    const auto parse = std::from_chars(begin, end, length);
    if (parse.ec != std::errc{} || parse.ptr != end ||
        length > requestId.size() - separator - 1) {
      parts.workflowPath.clear();
      parts.base = requestId;
      return parts;
    }
    cursor = separator + 1;
    parts.workflowPath.emplace_back(requestId.substr(cursor, length));
    cursor += length;
  }
  return parts;
}

std::string makeRequestId(const std::string &base, const std::vector<std::string> &workflowPath) {
  std::string requestId = base;
  requestId.append(kWorkflowPathMarker);
  for (const auto &name : workflowPath) {
    requestId.append(std::format("{}:{}", name.size(), name));
  }
  return requestId;
}

bool contextCancelled(const grpc::ServerContext *context) {
  return context != nullptr && context->IsCancelled();
}

bool contextDeadlineExceeded(const grpc::ServerContext *context) {
  return context != nullptr && context->deadline() != std::chrono::system_clock::time_point::max() &&
         std::chrono::system_clock::now() >= context->deadline();
}

grpc::Status contextStopStatus(const grpc::ServerContext *context) {
  if (contextDeadlineExceeded(context)) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "上游编排请求已超时");
  }
  return grpc::Status(grpc::StatusCode::CANCELLED, "上游编排请求已取消");
}

std::optional<std::chrono::steady_clock::time_point> makeContextDeadline(
    const grpc::ServerContext *context) {
  if (context == nullptr || context->deadline() == std::chrono::system_clock::time_point::max()) {
    return std::nullopt;
  }
  const auto remaining = context->deadline() - std::chrono::system_clock::now();
  return std::chrono::steady_clock::now() +
         std::chrono::duration_cast<std::chrono::steady_clock::duration>(remaining);
}

bool deadlineReached(const std::optional<std::chrono::steady_clock::time_point> &deadline) {
  return deadline.has_value() && std::chrono::steady_clock::now() >= *deadline;
}

bool interruptibleSleep(std::chrono::milliseconds duration,
                        const std::optional<std::chrono::steady_clock::time_point> &deadline,
                        const grpc::ServerContext *context) {
  auto wakeAt = std::chrono::steady_clock::now() + duration;
  if (deadline.has_value()) {
    wakeAt = std::min(wakeAt, *deadline);
  }
  while (std::chrono::steady_clock::now() < wakeAt) {
    if (contextCancelled(context) || contextDeadlineExceeded(context)) {
      return false;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        wakeAt - std::chrono::steady_clock::now());
    std::this_thread::sleep_for(std::min(kInterruptCheckInterval, std::max(std::chrono::milliseconds(1), remaining)));
  }
  return !contextCancelled(context) && !contextDeadlineExceeded(context) && !deadlineReached(deadline);
}

std::optional<std::chrono::steady_clock::time_point> makeSequenceDeadline(
    uint32_t timeoutMs, const grpc::ServerContext *context) {
  std::optional<std::chrono::steady_clock::time_point> deadline;
  if (timeoutMs > 0) {
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  }
  if (auto contextDeadline = makeContextDeadline(context); contextDeadline.has_value()) {
    deadline = deadline.has_value() ? std::min(deadline, contextDeadline) : contextDeadline;
  }
  return deadline;
}

DataCenterProto::CommandStatus commandStatusForGrpc(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::INVALID_ARGUMENT:
      return DataCenterProto::COMMAND_REJECTED;
    case grpc::StatusCode::NOT_FOUND:
      return DataCenterProto::COMMAND_NO_ROUTE;
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return DataCenterProto::COMMAND_TIMEOUT;
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::UNIMPLEMENTED:
      return DataCenterProto::COMMAND_TARGET_UNAVAILABLE;
    default:
      return DataCenterProto::COMMAND_INTERNAL_ERROR;
  }
}
}  // namespace

SequenceManager::SequenceManager(std::filesystem::path configDbPath) :
  store_(std::move(configDbPath)),
  dataCenter_("ControlOrchestrator") {}

void SequenceManager::setDataCenterServerAddress(std::string address) {
  dataCenter_.setServerAddress(std::move(address));
}

void SequenceManager::setDataCenterStub(
    std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  dataCenter_.setStub(std::move(stub));
}

grpc::Status SequenceManager::LoadPersistedConfig() {
  ControlOrchestratorProto::SequencesConfig config;
  auto status = store_.Load(&config);
  if (!status.ok()) {
    LOG_ERROR("ControlOrchestrator 恢复编排配置失败: {}", status.error_message());
    return status;
  }
  std::lock_guard<std::mutex> lock(mu_);
  sequences_.clear();
  for (const auto &sequence : config.configs()) {
    sequences_[sequence.sequence_name()] = sequence;
    sequenceLocks_[sequence.sequence_name()] = std::make_shared<std::mutex>();
  }
  LOG_INFO("ControlOrchestrator 已恢复编排配置: 数量={}", sequences_.size());
  return grpc::Status::OK;
}

grpc::Status SequenceManager::UpsertSequence(
    const ControlOrchestratorProto::WorkflowConfig &config,
    bool createOnly,
    ControlOrchestratorProto::WorkflowConfig *out) {
  if (out == nullptr) {
    return invalid("响应为空");
  }
  auto status = ValidateWorkflowConfig(config);
  if (!status.ok()) {
    LOG_ERROR("ControlOrchestrator 编排配置校验失败: {}", status.error_message());
    return status;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (createOnly && sequences_.contains(config.sequence_name())) {
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "编排名称已存在");
  }
  sequences_[config.sequence_name()] = config;
  if (!sequenceLocks_.contains(config.sequence_name())) {
    sequenceLocks_[config.sequence_name()] = std::make_shared<std::mutex>();
  }
  status = saveLocked();
  if (!status.ok()) {
    LOG_ERROR("ControlOrchestrator 保存编排配置失败: {}", status.error_message());
    return status;
  }
  *out = config;
  LOG_INFO("ControlOrchestrator 已保存编排: name={}, steps={}",
           config.sequence_name(), config.steps_size());
  return grpc::Status::OK;
}

grpc::Status SequenceManager::GetSequence(const std::string &name,
                                           ControlOrchestratorProto::WorkflowConfig *out) const {
  if (out == nullptr || name.empty()) {
    return invalid("name 不能为空");
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sequences_.find(name);
  if (it == sequences_.end()) {
    return notFound(name);
  }
  *out = it->second;
  return grpc::Status::OK;
}

grpc::Status SequenceManager::ListSequences(
    ControlOrchestratorProto::ListSequencesResponse *out) const {
  if (out == nullptr) {
    return invalid("响应为空");
  }
  std::lock_guard<std::mutex> lock(mu_);
  out->Clear();
  for (const auto &[_, sequence] : sequences_) {
    *out->add_configs() = sequence;
  }
  return grpc::Status::OK;
}

grpc::Status SequenceManager::DeleteSequence(const std::string &name) {
  if (name.empty()) {
    return invalid("sequence_name 不能为空");
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!sequences_.contains(name)) {
    return notFound(name);
  }
  sequences_.erase(name);
  auto status = saveLocked();
  if (!status.ok()) {
    LOG_ERROR("ControlOrchestrator 删除编排后保存配置失败: {}", status.error_message());
    return status;
  }
  LOG_INFO("ControlOrchestrator 已删除编排: name={}", name);
  return grpc::Status::OK;
}

grpc::Status SequenceManager::ExecuteSequence(
    const ControlOrchestratorProto::ExecuteSequenceRequest &request,
    ControlOrchestratorProto::ExecuteSequenceResponse *response,
    grpc::ServerContext *serverContext) {
  if (response == nullptr) {
    return invalid("响应为空");
  }
  response->Clear();
  if (request.sequence_name().empty()) {
    return invalid("sequence_name 不能为空");
  }
  if (contextCancelled(serverContext) || contextDeadlineExceeded(serverContext)) {
    return contextStopStatus(serverContext);
  }
  const auto requestIdParts = splitRequestId(request.request_id());
  if (requestIdParts.workflowPath.size() >= kMaxWorkflowPath) {
    response->set_reason("控制编排调用层级超过上限");
    response->set_failed_command_status(DataCenterProto::COMMAND_INTERNAL_ERROR);
    LOG_ERROR("ControlOrchestrator 编排调用层级超过上限: name={}, request_id={}",
              request.sequence_name(), request.request_id());
    return grpc::Status::OK;
  }
  if (std::find(requestIdParts.workflowPath.begin(), requestIdParts.workflowPath.end(),
                request.sequence_name()) != requestIdParts.workflowPath.end()) {
    response->set_reason("检测到控制编排递归调用");
    response->set_failed_command_status(DataCenterProto::COMMAND_INTERNAL_ERROR);
    LOG_ERROR("ControlOrchestrator 检测到编排递归调用: name={}, request_id={}",
              request.sequence_name(), request.request_id());
    return grpc::Status::OK;
  }
  ControlOrchestratorProto::WorkflowConfig config;
  std::shared_ptr<std::mutex> sequenceLock;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sequences_.find(request.sequence_name());
    if (it == sequences_.end()) {
      return notFound(request.sequence_name());
    }
    config = it->second;
    sequenceLock = lockForSequenceLocked(request.sequence_name());
  }
  const auto sequenceDeadline = makeSequenceDeadline(request.timeout_ms(), serverContext);
  std::unique_lock<std::mutex> executionLock(*sequenceLock, std::defer_lock);
  while (!executionLock.try_lock()) {
    if (contextCancelled(serverContext) || contextDeadlineExceeded(serverContext)) {
      return contextStopStatus(serverContext);
    }
    if (deadlineReached(sequenceDeadline)) {
      response->set_reason("编排总超时");
      response->set_failed_command_status(DataCenterProto::COMMAND_TIMEOUT);
      return grpc::Status::OK;
    }
    if (!interruptibleSleep(kInterruptCheckInterval, sequenceDeadline, serverContext)) {
      if (contextCancelled(serverContext) || contextDeadlineExceeded(serverContext)) {
        return contextStopStatus(serverContext);
      }
      response->set_reason("编排总超时");
      response->set_failed_command_status(DataCenterProto::COMMAND_TIMEOUT);
      return grpc::Status::OK;
    }
  }
  LOG_INFO("ControlOrchestrator 开始执行编排: name={}, request_id={}",
           request.sequence_name(), request.request_id());

  auto failStep = [&](int index, const std::string &reason,
                      DataCenterProto::CommandStatus commandStatus) {
    const auto &step = config.steps(index);
    response->set_failed_step_index(static_cast<uint32_t>(index + 1));
    response->set_failed_step_name(step.step_name());
    response->set_reason(reason);
    response->set_failed_command_status(commandStatus);
    LOG_ERROR("ControlOrchestrator 编排执行失败: name={}, step={}, 原因={}",
              request.sequence_name(), step.step_name(), reason);
  };

  for (int index = 0; index < config.steps_size(); ++index) {
    const auto &step = config.steps(index);
    if (contextCancelled(serverContext) || contextDeadlineExceeded(serverContext)) {
      return contextStopStatus(serverContext);
    }
    if (deadlineReached(sequenceDeadline)) {
      response->set_failed_step_index(static_cast<uint32_t>(index + 1));
      response->set_failed_step_name(step.step_name());
      response->set_reason("编排总超时");
      response->set_failed_command_status(DataCenterProto::COMMAND_TIMEOUT);
      LOG_ERROR("ControlOrchestrator 编排执行失败: name={}, step={}, 原因=编排总超时",
                request.sequence_name(), step.step_name());
      return grpc::Status::OK;
    }
    bool stepAccepted = false;
    const auto retryCount = step.has_verification() &&
                                    step.verification().failure_action() ==
                                        ControlOrchestratorProto::StepVerification::RETRY_COMMAND
                                ? step.verification().max_retries()
                                : 0u;
    for (uint32_t attempt = 0; attempt <= retryCount && !stepAccepted; ++attempt) {
      if (contextCancelled(serverContext) || contextDeadlineExceeded(serverContext)) {
        return contextStopStatus(serverContext);
      }
      if (deadlineReached(sequenceDeadline)) {
        failStep(index, "编排总超时", DataCenterProto::COMMAND_TIMEOUT);
        return grpc::Status::OK;
      }
      DataCenterProto::ExecuteCommandRequest command;
      *command.mutable_src() = step.source();
      LOG_INFO("ControlOrchestrator 开始执行步骤: name={}, index={}, step={}, attempt={}, source={}/{}/{}",
               request.sequence_name(), index + 1, step.step_name(), attempt + 1,
               step.source().module_name(), step.source().conn_name(), step.source().tag());
      if (step.use_trigger_value()) {
        if (request.trigger_value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
          failStep(index, "步骤使用触发值但请求未提供 trigger_value",
                   DataCenterProto::COMMAND_REJECTED);
          return grpc::Status::OK;
        }
        *command.mutable_value() = request.trigger_value();
      } else {
        *command.mutable_value() = step.value();
      }
      auto commandTimeout = step.timeout_ms();
      if (sequenceDeadline.has_value()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            *sequenceDeadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
          failStep(index, "编排总超时", DataCenterProto::COMMAND_TIMEOUT);
          return grpc::Status::OK;
        }
        commandTimeout = std::min<uint32_t>(
            commandTimeout, static_cast<uint32_t>(std::min<int64_t>(remaining.count(), UINT32_MAX)));
      }
      command.set_timeout_ms(commandTimeout);
      auto stepRequestBase = requestIdParts.base.empty() ? request.sequence_name() : requestIdParts.base;
      stepRequestBase = std::format("{}-{}-{}", stepRequestBase, index + 1, attempt + 1);
      auto workflowPath = requestIdParts.workflowPath;
      workflowPath.emplace_back(request.sequence_name());
      command.set_request_id(makeRequestId(stepRequestBase, workflowPath));

      DataCenterProto::ExecuteCommandResponse commandResponse;
      auto status = dataCenter_.Execute(command, &commandResponse, serverContext);
      if (!status.ok()) {
        if (contextCancelled(serverContext) || contextDeadlineExceeded(serverContext)) {
          return contextStopStatus(serverContext);
        }
        const auto commandStatus = commandStatusForGrpc(status.error_code());
        failStep(index, std::format("DataCenter 调用失败: {}", status.error_message()), commandStatus);
        return grpc::Status::OK;
      }
      if (commandResponse.status() == DataCenterProto::COMMAND_STATUS_UNSPECIFIED) {
        failStep(index, "DataCenter 返回未定义命令状态", DataCenterProto::COMMAND_INTERNAL_ERROR);
        return grpc::Status::OK;
      }
      if (commandResponse.status() != DataCenterProto::COMMAND_ACCEPTED) {
        failStep(index, commandResponse.reason().empty() ? "目标模块未接受命令" : commandResponse.reason(),
                 commandResponse.status());
        return grpc::Status::OK;
      }

      if (step.has_verification()) {
        const auto &verification = step.verification();
        bool verified = false;
        auto verificationDeadline = std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(verification.wait_timeout_ms());
        if (sequenceDeadline.has_value()) {
          verificationDeadline = std::min(verificationDeadline, *sequenceDeadline);
        }
        while (std::chrono::steady_clock::now() <= verificationDeadline) {
          if (contextCancelled(serverContext) || contextDeadlineExceeded(serverContext)) {
            return contextStopStatus(serverContext);
          }
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
              verificationDeadline - std::chrono::steady_clock::now());
          if (remaining.count() <= 0) {
            break;
          }
          DataCenterProto::GetSourceLatestResponse latest;
          auto latestStatus = dataCenter_.GetSourceLatest(
              verification.status_source(), &latest,
              std::max(std::chrono::milliseconds(1), remaining), serverContext);
          if (!latestStatus.ok()) {
            if (contextCancelled(serverContext) || contextDeadlineExceeded(serverContext)) {
              return contextStopStatus(serverContext);
            }
            LOG_WARNING("ControlOrchestrator 遥信读取失败: name={}, step={}, 原因={}",
                        request.sequence_name(), step.step_name(), latestStatus.error_message());
          } else if (latest.updates_size() > 0 && latest.updates(0).value().kind_case() ==
                         DataCenterProto::PointValue::kBoolValue &&
                     latest.updates(0).value().bool_value() == verification.expected_value().bool_value()) {
            verified = true;
            break;
          }
          if (!interruptibleSleep(std::chrono::milliseconds(verification.poll_interval_ms()),
                                  verificationDeadline, serverContext)) {
            if (contextCancelled(serverContext) || contextDeadlineExceeded(serverContext)) {
              return contextStopStatus(serverContext);
            }
            break;
          }
        }
        if (verified) {
          LOG_INFO("ControlOrchestrator 遥信确认成功: name={}, step={}",
                   request.sequence_name(), step.step_name());
          stepAccepted = true;
        } else if (attempt < retryCount) {
          LOG_WARNING("ControlOrchestrator 遥信确认失败，准备重试前置命令: name={}, step={}, retry={}/{}",
                      request.sequence_name(), step.step_name(), attempt + 1, retryCount);
          if (verification.retry_interval_ms() > 0) {
            if (!interruptibleSleep(std::chrono::milliseconds(verification.retry_interval_ms()),
                                    sequenceDeadline, serverContext)) {
              if (contextCancelled(serverContext) || contextDeadlineExceeded(serverContext)) {
                return contextStopStatus(serverContext);
              }
              failStep(index, "编排总超时", DataCenterProto::COMMAND_TIMEOUT);
              return grpc::Status::OK;
            }
          }
        } else {
          failStep(index, "遥信状态在超时时间内未达到期望值", DataCenterProto::COMMAND_TIMEOUT);
          return grpc::Status::OK;
        }
      } else {
        stepAccepted = true;
      }
    }
    response->set_executed_steps(response->executed_steps() + 1);
    LOG_INFO("ControlOrchestrator 步骤执行成功: name={}, index={}, step={}",
             request.sequence_name(), index + 1, step.step_name());
    if (step.delay_after_ms() > 0 && index + 1 < config.steps_size()) {
      auto delay = std::chrono::milliseconds(step.delay_after_ms());
      if (sequenceDeadline.has_value()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            *sequenceDeadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
          response->set_failed_step_index(static_cast<uint32_t>(index + 2));
          response->set_failed_step_name(config.steps(index + 1).step_name());
          response->set_reason("编排总超时");
          response->set_failed_command_status(DataCenterProto::COMMAND_TIMEOUT);
          LOG_ERROR("ControlOrchestrator 编排执行失败: name={}, 原因=步骤间延时后总超时",
                    request.sequence_name());
          return grpc::Status::OK;
        }
        delay = std::min(delay, remaining);
      }
      if (!interruptibleSleep(delay, sequenceDeadline, serverContext)) {
        if (contextCancelled(serverContext) || contextDeadlineExceeded(serverContext)) {
          return contextStopStatus(serverContext);
        }
        response->set_failed_step_index(static_cast<uint32_t>(index + 2));
        response->set_failed_step_name(config.steps(index + 1).step_name());
        response->set_reason("编排总超时");
        response->set_failed_command_status(DataCenterProto::COMMAND_TIMEOUT);
        return grpc::Status::OK;
      }
    }
  }
  response->set_accepted(true);
  response->set_reason("编排执行成功");
  LOG_INFO("ControlOrchestrator 编排执行成功: name={}, steps={}",
           request.sequence_name(), config.steps_size());
  return grpc::Status::OK;
}

grpc::Status SequenceManager::ExecuteTriggeredCommand(
    const DataCenterProto::ExecuteCommandRequest &request,
    DataCenterProto::ExecuteCommandResponse *response,
    grpc::ServerContext *serverContext) {
  if (response == nullptr) {
    return invalid("响应为空");
  }
  response->Clear();
  if (!request.has_src() || request.src().tag().empty() ||
      request.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
    return invalid("触发命令缺少源端点或命令值");
  }
  ControlOrchestratorProto::WorkflowConfig config;
  size_t matchedCount = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto &[_, candidate] : sequences_) {
      if (!candidate.has_trigger()) {
        continue;
      }
      const auto &trigger = candidate.trigger();
      const bool stableMatch = !request.src().module_name().empty() &&
                               !request.src().conn_name().empty() &&
                               trigger.module_name() == request.src().module_name() &&
                               trigger.conn_name() == request.src().conn_name() &&
                               trigger.tag() == request.src().tag();
      const bool idMatch = trigger.conn_id() != 0 && request.src().conn_id() != 0 &&
                           trigger.conn_id() == request.src().conn_id() &&
                           trigger.tag() == request.src().tag();
      if (stableMatch || idMatch) {
        ++matchedCount;
        config = candidate;
      }
    }
  }
  if (matchedCount > 1) {
    response->set_status(DataCenterProto::COMMAND_AMBIGUOUS_ROUTE);
    response->set_reason("触发源点绑定了多个控制编排");
    LOG_ERROR("ControlOrchestrator 触发源点绑定歧义: source={}/{}/{}",
              request.src().module_name(), request.src().conn_name(), request.src().tag());
    return grpc::Status::OK;
  }
  if (config.sequence_name().empty()) {
    response->set_status(DataCenterProto::COMMAND_NO_ROUTE);
    response->set_reason("触发源点未绑定控制编排");
    LOG_WARNING("ControlOrchestrator 未找到触发源点绑定: source={}/{}/{}",
                request.src().module_name(), request.src().conn_name(), request.src().tag());
    return grpc::Status::OK;
  }
  ControlOrchestratorProto::ExecuteSequenceRequest sequenceRequest;
  sequenceRequest.set_sequence_name(config.sequence_name());
  *sequenceRequest.mutable_trigger() = request.src();
  *sequenceRequest.mutable_trigger_value() = request.value();
  sequenceRequest.set_request_id(request.request_id());
  sequenceRequest.set_timeout_ms(request.timeout_ms());
  ControlOrchestratorProto::ExecuteSequenceResponse sequenceResponse;
  auto status = ExecuteSequence(sequenceRequest, &sequenceResponse, serverContext);
  if (!status.ok()) {
    return status;
  }
  if (sequenceResponse.accepted()) {
    response->set_status(DataCenterProto::COMMAND_ACCEPTED);
    response->set_reason("控制编排执行成功");
  } else {
    const auto failureStatus = sequenceResponse.failed_command_status();
    response->set_status(failureStatus == DataCenterProto::COMMAND_STATUS_UNSPECIFIED
                             ? DataCenterProto::COMMAND_REJECTED
                             : failureStatus);
    if (response->status() == DataCenterProto::COMMAND_REJECTED) {
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
    }
    response->set_reason(sequenceResponse.reason());
  }
  LOG_INFO("ControlOrchestrator 触发编排处理完成: name={}, accepted={}, reason={}",
           config.sequence_name(), sequenceResponse.accepted(), sequenceResponse.reason());
  return grpc::Status::OK;
}

grpc::Status SequenceManager::saveLocked() {
  ControlOrchestratorProto::SequencesConfig config;
  for (const auto &[_, sequence] : sequences_) {
    *config.add_configs() = sequence;
  }
  return store_.Save(config);
}

std::shared_ptr<std::mutex> SequenceManager::lockForSequenceLocked(const std::string &name) {
  auto &lock = sequenceLocks_[name];
  if (!lock) {
    lock = std::make_shared<std::mutex>();
  }
  return lock;
}

}  // namespace ControlOrchestrator
