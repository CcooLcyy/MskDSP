#include "ConfigPusherApplyControlOrchestrator.h"

#include <unordered_set>

#include "Logger.h"
#include "ProtoLogUtil.hpp"

namespace ConfigPusher {

bool applyControlOrchestratorConfig(
    const ConfigPusherProto::ControlOrchestratorConfig &config,
    ControlOrchestratorProto::ControlOrchestratorService::StubInterface *stub) {
  if (stub == nullptr) {
    LOG_ERROR("ControlOrchestrator gRPC stub 为空");
    return false;
  }
  std::unordered_set<std::string> desired;
  for (const auto &sequence : config.sequences()) {
    if (sequence.sequence_name().empty() || !desired.emplace(sequence.sequence_name()).second) {
      LOG_ERROR("ControlOrchestrator 配置存在空名称或重复名称");
      return false;
    }
    ControlOrchestratorProto::UpsertSequenceRequest request;
    *request.mutable_config() = sequence;
    request.set_create_only(false);
    ControlOrchestratorProto::WorkflowConfig response;
    grpc::ClientContext context;
    auto status = stub->UpsertSequence(&context, request, &response);
    if (!status.ok()) {
      LOG_ERROR("ControlOrchestrator 下发编排失败: name={}, 原因={}",
                sequence.sequence_name(), status.error_message());
      return false;
    }
  }

  ControlOrchestratorProto::ListSequencesRequest listRequest;
  ControlOrchestratorProto::ListSequencesResponse listResponse;
  grpc::ClientContext listContext;
  auto status = stub->ListSequences(&listContext, listRequest, &listResponse);
  if (!status.ok()) {
    LOG_ERROR("ControlOrchestrator 获取现有编排失败: {}", status.error_message());
    return false;
  }
  for (const auto &existing : listResponse.configs()) {
    if (desired.contains(existing.sequence_name())) {
      continue;
    }
    ControlOrchestratorProto::DeleteSequenceRequest deleteRequest;
    deleteRequest.set_sequence_name(existing.sequence_name());
    DataCenterProto::Empty deleteResponse;
    grpc::ClientContext deleteContext;
    status = stub->DeleteSequence(&deleteContext, deleteRequest, &deleteResponse);
    if (!status.ok()) {
      LOG_ERROR("ControlOrchestrator 删除旧编排失败: name={}, 原因={}",
                existing.sequence_name(), status.error_message());
      return false;
    }
  }
  LOG_INFO("ControlOrchestrator 配置下发完成: 编排数={}", config.sequences_size());
  return true;
}

}  // namespace ConfigPusher
