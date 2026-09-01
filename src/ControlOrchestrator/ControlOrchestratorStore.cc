#include "ControlOrchestratorStore.h"

#include <format>
#include <unordered_set>

#include "Logger.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace ControlOrchestrator {
namespace {
grpc::Status invalid(std::string reason) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(reason));
}

void trace(const std::string &message) {
  LOG_INFO("{}", message);
}
}  // namespace

grpc::Status ValidateWorkflowConfig(const ControlOrchestratorProto::WorkflowConfig &config) {
  if (config.sequence_name().empty()) {
    return invalid("sequence_name 不能为空");
  }
  if (config.steps_size() < 1 || config.steps_size() > 8) {
    return invalid("steps 数量必须在 1 到 8 之间");
  }
  std::unordered_set<std::string> names;
  if (config.has_trigger()) {
    const auto &trigger = config.trigger();
    if (trigger.module_name().empty() || trigger.conn_name().empty() || trigger.tag().empty()) {
      return invalid("trigger.module_name/conn_name/tag 不能为空");
    }
  }
  for (const auto &step : config.steps()) {
    if (step.step_name().empty()) {
      return invalid("step_name 不能为空");
    }
    if (!names.emplace(step.step_name()).second) {
      return invalid(std::format("step_name 重复: {}", step.step_name()));
    }
    if (!step.has_source() || step.source().tag().empty()) {
      return invalid("步骤 source.tag 不能为空");
    }
    if (step.source().module_name().empty() || step.source().conn_name().empty()) {
      return invalid("步骤 source.module_name/conn_name 不能为空");
    }
    if (!step.use_trigger_value() && step.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
      return invalid(std::format("步骤 {} 未提供固定值", step.step_name()));
    }
    if (step.timeout_ms() == 0) {
      return invalid(std::format("步骤 {} timeout_ms 必须大于 0", step.step_name()));
    }
    const bool sameTriggerStable = config.has_trigger() &&
                                   step.source().module_name() == config.trigger().module_name() &&
                                   step.source().conn_name() == config.trigger().conn_name() &&
                                   step.source().tag() == config.trigger().tag();
    const bool sameTriggerId = config.has_trigger() && config.trigger().conn_id() != 0 &&
                               step.source().conn_id() != 0 &&
                               step.source().conn_id() == config.trigger().conn_id() &&
                               step.source().tag() == config.trigger().tag();
    if (sameTriggerStable || sameTriggerId) {
      return invalid(std::format("步骤 {} 不能使用编排触发源点，否则会递归执行", step.step_name()));
    }
    if (step.has_verification()) {
      const auto &verification = step.verification();
      if (!verification.has_status_source() || verification.status_source().tag().empty() ||
          (verification.status_source().conn_id() == 0 &&
           (verification.status_source().module_name().empty() ||
            verification.status_source().conn_name().empty()))) {
        return invalid(std::format("步骤 {} 遥信状态端点必须包含连接标识和 tag", step.step_name()));
      }
      if (verification.expected_value().kind_case() != DataCenterProto::PointValue::kBoolValue) {
        return invalid(std::format("步骤 {} 遥信期望值必须为 BOOL", step.step_name()));
      }
      if (verification.wait_timeout_ms() == 0 || verification.poll_interval_ms() == 0) {
        return invalid(std::format("步骤 {} 遥信等待超时和轮询间隔必须大于 0", step.step_name()));
      }
      if (verification.failure_action() != ControlOrchestratorProto::StepVerification::STOP &&
          verification.failure_action() != ControlOrchestratorProto::StepVerification::RETRY_COMMAND) {
        return invalid(std::format("步骤 {} 遥信失败策略无效", step.step_name()));
      }
      if (verification.max_retries() > 100) {
        return invalid(std::format("步骤 {} max_retries 不能超过 100", step.step_name()));
      }
    }
  }
  return grpc::Status::OK;
}

grpc::Status ValidateSequencesConfig(const ControlOrchestratorProto::SequencesConfig &config) {
  std::unordered_set<std::string> names;
  for (const auto &sequence : config.configs()) {
    auto status = ValidateWorkflowConfig(sequence);
    if (!status.ok()) {
      return status;
    }
    if (!names.emplace(sequence.sequence_name()).second) {
      return invalid(std::format("sequence_name 重复: {}", sequence.sequence_name()));
    }
  }
  return grpc::Status::OK;
}

SequenceStore::SequenceStore(std::filesystem::path configDbPath) :
  configDbPath_(std::move(configDbPath)) {}

grpc::Status SequenceStore::Save(const ControlOrchestratorProto::SequencesConfig &config) {
  mskdsp::detail::ProtoSqliteStore<ControlOrchestratorProto::SequencesConfig> store(
      configDbPath_, "ControlOrchestrator", "sequences",
      "ControlOrchestratorProto.SequencesConfig", ValidateSequencesConfig, trace);
  return store.Save(config);
}

grpc::Status SequenceStore::Load(ControlOrchestratorProto::SequencesConfig *out) {
  mskdsp::detail::ProtoSqliteStore<ControlOrchestratorProto::SequencesConfig> store(
      configDbPath_, "ControlOrchestrator", "sequences",
      "ControlOrchestratorProto.SequencesConfig", ValidateSequencesConfig, trace);
  return store.Load(out);
}

}  // namespace ControlOrchestrator
