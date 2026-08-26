#include "ControlOrchestratorManager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <thread>
#include <utility>

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
    ControlOrchestratorProto::ExecuteSequenceResponse *response) {
  if (response == nullptr) {
    return invalid("响应为空");
  }
  response->Clear();
  if (request.sequence_name().empty()) {
    return invalid("sequence_name 不能为空");
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
  std::unique_lock<std::mutex> executionLock(*sequenceLock);
  LOG_INFO("ControlOrchestrator 开始执行编排: name={}, request_id={}",
           request.sequence_name(), request.request_id());

  const auto sequenceDeadline = request.timeout_ms() == 0
                                    ? std::optional<std::chrono::steady_clock::time_point>{}
                                    : std::optional<std::chrono::steady_clock::time_point>{
                                          std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(request.timeout_ms())};

  for (int index = 0; index < config.steps_size(); ++index) {
    const auto &step = config.steps(index);
    if (sequenceDeadline.has_value() &&
        std::chrono::steady_clock::now() >= *sequenceDeadline) {
      response->set_failed_step_index(static_cast<uint32_t>(index + 1));
      response->set_failed_step_name(step.step_name());
      response->set_reason("编排总超时");
      LOG_ERROR("ControlOrchestrator 编排执行失败: name={}, step={}, 原因=编排总超时",
                request.sequence_name(), step.step_name());
      return grpc::Status::OK;
    }
    DataCenterProto::ExecuteCommandRequest command;
    *command.mutable_src() = step.source();
    LOG_INFO("ControlOrchestrator 开始执行步骤: name={}, index={}, step={}, source={}/{}/{}",
             request.sequence_name(), index + 1, step.step_name(),
             step.source().module_name(), step.source().conn_name(), step.source().tag());
    if (step.use_trigger_value()) {
      if (request.trigger_value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
        response->set_failed_step_index(static_cast<uint32_t>(index + 1));
        response->set_failed_step_name(step.step_name());
        response->set_reason("步骤使用触发值但请求未提供 trigger_value");
        LOG_ERROR("ControlOrchestrator 编排执行失败: {}", response->reason());
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
        response->set_failed_step_index(static_cast<uint32_t>(index + 1));
        response->set_failed_step_name(step.step_name());
        response->set_reason("编排总超时");
        LOG_ERROR("ControlOrchestrator 编排执行失败: name={}, step={}, 原因=编排总超时",
                  request.sequence_name(), step.step_name());
        return grpc::Status::OK;
      }
      commandTimeout = std::min<uint32_t>(
          commandTimeout, static_cast<uint32_t>(std::min<int64_t>(remaining.count(), UINT32_MAX)));
    }
    command.set_timeout_ms(commandTimeout);
    command.set_request_id(request.request_id().empty()
                              ? std::format("{}-{}", request.sequence_name(), index + 1)
                              : std::format("{}-{}", request.request_id(), index + 1));

    DataCenterProto::ExecuteCommandResponse commandResponse;
    auto status = dataCenter_.Execute(command, &commandResponse);
    if (!status.ok()) {
      response->set_failed_step_index(static_cast<uint32_t>(index + 1));
      response->set_failed_step_name(step.step_name());
      response->set_reason(std::format("DataCenter 调用失败: {}", status.error_message()));
      LOG_ERROR("ControlOrchestrator 步骤执行失败: name={}, step={}, 原因={}",
                request.sequence_name(), step.step_name(), response->reason());
      return grpc::Status::OK;
    }
    if (commandResponse.status() == DataCenterProto::COMMAND_STATUS_UNSPECIFIED) {
      response->set_failed_step_index(static_cast<uint32_t>(index + 1));
      response->set_failed_step_name(step.step_name());
      response->set_reason("DataCenter 返回未定义命令状态");
      LOG_ERROR("ControlOrchestrator 步骤状态未定义: name={}, step={}",
                request.sequence_name(), step.step_name());
      return grpc::Status::OK;
    }
    if (commandResponse.status() != DataCenterProto::COMMAND_ACCEPTED) {
      response->set_failed_step_index(static_cast<uint32_t>(index + 1));
      response->set_failed_step_name(step.step_name());
      response->set_reason(commandResponse.reason().empty()
                               ? "目标模块未接受命令"
                               : commandResponse.reason());
      LOG_ERROR("ControlOrchestrator 步骤被拒绝: name={}, step={}, 原因={}",
                request.sequence_name(), step.step_name(), response->reason());
      return grpc::Status::OK;
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
          LOG_ERROR("ControlOrchestrator 编排执行失败: name={}, 原因=步骤间延时后总超时",
                    request.sequence_name());
          return grpc::Status::OK;
        }
        delay = std::min(delay, remaining);
      }
      std::this_thread::sleep_for(delay);
    }
  }
  response->set_accepted(true);
  response->set_reason("编排执行成功");
  LOG_INFO("ControlOrchestrator 编排执行成功: name={}, steps={}",
           request.sequence_name(), config.steps_size());
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
