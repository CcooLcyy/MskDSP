#include "ControlOrchestratorGrpcService.h"

#include "ControlOrchestratorManager.h"
#include "Logger.h"

namespace ControlOrchestrator {

void GrpcServiceImpl::setManager(SequenceManager *manager) {
  manager_ = manager;
}

grpc::Status GrpcServiceImpl::UpsertSequence(
    grpc::ServerContext *, const ControlOrchestratorProto::UpsertSequenceRequest *request,
    ControlOrchestratorProto::WorkflowConfig *response) {
  if (manager_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr || !request->has_config()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求或配置为空");
  }
  auto status = manager_->UpsertSequence(request->config(), request->create_only(), response);
  if (!status.ok()) {
    LOG_ERROR("ControlOrchestrator 新增/更新编排失败: {}", status.error_message());
  }
  return status;
}

grpc::Status GrpcServiceImpl::GetSequence(
    grpc::ServerContext *, const ControlOrchestratorProto::GetSequenceRequest *request,
    ControlOrchestratorProto::WorkflowConfig *response) {
  if (manager_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求或响应为空");
  }
  return manager_->GetSequence(request->sequence_name(), response);
}

grpc::Status GrpcServiceImpl::ListSequences(
    grpc::ServerContext *, const ControlOrchestratorProto::ListSequencesRequest *,
    ControlOrchestratorProto::ListSequencesResponse *response) {
  if (manager_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  return manager_->ListSequences(response);
}

grpc::Status GrpcServiceImpl::DeleteSequence(
    grpc::ServerContext *, const ControlOrchestratorProto::DeleteSequenceRequest *request,
    DataCenterProto::Empty *) {
  if (manager_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  return manager_->DeleteSequence(request->sequence_name());
}

grpc::Status GrpcServiceImpl::ExecuteSequence(
    grpc::ServerContext *, const ControlOrchestratorProto::ExecuteSequenceRequest *request,
    ControlOrchestratorProto::ExecuteSequenceResponse *response) {
  if (manager_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求或响应为空");
  }
  return manager_->ExecuteSequence(*request, response);
}

}  // namespace ControlOrchestrator
