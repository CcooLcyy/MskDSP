#include "IEC61850GrpcService.h"

#include "IEC61850Manager.h"
#include "Logger.h"

namespace IEC61850 {

void IEC61850GrpcServiceImpl::SetManager(Manager* manager) {
  manager_ = manager;
}

grpc::Status IEC61850GrpcServiceImpl::ApplyTargetConfig(
    grpc::ServerContext*,
    const IEC61850Proto::ApplyTargetConfigRequest* request,
    IEC61850Proto::ApplyTargetConfigResponse* response) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "请求和响应不能为空");
  }
  LOG_INFO("IEC61850收到完整目标态: 模型数={}, IED数={}",
           request->models_size(), request->ieds_size());
  const auto status = manager_->ApplyTargetConfig(*request, response);
  if (!status.ok()) {
    LOG_ERROR("IEC61850应用完整目标态失败: 模型数={}, IED数={}, 原因={}",
              request->models_size(), request->ieds_size(),
              status.error_message());
  }
  return status;
}

grpc::Status IEC61850GrpcServiceImpl::ImportScl(
    grpc::ServerContext*, const IEC61850Proto::ImportSclRequest* request,
    IEC61850Proto::ImportSclResponse* response) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "请求和响应不能为空");
  }
  LOG_INFO("IEC61850收到SCL导入请求: 模型={}, 来源={}, 内容字节数={}, 仅校验={}, 替换={}",
           request->model_name(), request->source_name(), request->content().size(),
           request->validate_only(), request->replace());
  auto status = manager_->ImportScl(*request, response);
  if (!status.ok()) {
    LOG_ERROR("IEC61850导入SCL失败: 模型={}, 来源={}, 原因={}",
              request->model_name(), request->source_name(), status.error_message());
  } else {
    LOG_INFO("IEC61850导入SCL完成: 模型={}, IED数={}, 校验问题数={}",
             request->model_name(), response->summary().ied_count(),
             response->issues_size());
  }
  return status;
}

grpc::Status IEC61850GrpcServiceImpl::GetModelSummary(
    grpc::ServerContext*, const IEC61850Proto::GetModelSummaryRequest* request,
    IEC61850Proto::SclModelSummary* response) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "请求和响应不能为空");
  }
  const auto status = manager_->GetModelSummary(request->model_name(), response);
  if (status.ok()) {
    LOG_INFO("IEC61850返回模型目录摘要: 模型={}, IED数={}",
             request->model_name(), response->ieds_size());
  }
  return status;
}

grpc::Status IEC61850GrpcServiceImpl::ListModels(
    grpc::ServerContext*, const IEC61850Proto::Empty*,
    IEC61850Proto::ListModelsResponse* response) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  const auto status = manager_->ListModels(response);
  if (status.ok()) {
    LOG_INFO("IEC61850返回模型目录列表: 模型数={}", response->models_size());
  }
  return status;
}

grpc::Status IEC61850GrpcServiceImpl::DeleteModel(
    grpc::ServerContext*, const IEC61850Proto::DeleteModelRequest* request,
    IEC61850Proto::Empty*) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求不能为空");
  }
  auto status = manager_->DeleteModel(request->model_name());
  if (status.ok()) {
    LOG_INFO("IEC61850已删除模型: 模型={}", request->model_name());
  }
  return status;
}

grpc::Status IEC61850GrpcServiceImpl::UpsertIed(
    grpc::ServerContext*, const IEC61850Proto::UpsertIedRequest* request,
    IEC61850Proto::IedInfo* response) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "请求和响应不能为空");
  }
  auto status = manager_->UpsertIed(*request, response);
  if (!status.ok()) {
    LOG_ERROR("IEC61850配置IED失败: IED={}, 原因={}",
              request->config().conn_name(), status.error_message());
  } else {
    LOG_INFO("IEC61850配置IED完成: IED={}, 连接ID={}, DataCenter可用={}",
             response->config().conn_name(), response->conn_id(),
             response->data_center_available());
  }
  return status;
}

grpc::Status IEC61850GrpcServiceImpl::GetIed(
    grpc::ServerContext*, const IEC61850Proto::GetIedRequest* request,
    IEC61850Proto::IedInfo* response) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "请求和响应不能为空");
  }
  return manager_->GetIed(request->conn_name(), response);
}

grpc::Status IEC61850GrpcServiceImpl::ListIeds(
    grpc::ServerContext*, const IEC61850Proto::Empty*,
    IEC61850Proto::ListIedsResponse* response) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  return manager_->ListIeds(response);
}

grpc::Status IEC61850GrpcServiceImpl::DeleteIed(
    grpc::ServerContext*, const IEC61850Proto::DeleteIedRequest* request,
    IEC61850Proto::Empty*) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求不能为空");
  }
  auto status = manager_->DeleteIed(request->conn_name());
  if (status.ok()) {
    LOG_INFO("IEC61850已删除IED配置: IED={}", request->conn_name());
  }
  return status;
}

grpc::Status IEC61850GrpcServiceImpl::StartIed(
    grpc::ServerContext*, const IEC61850Proto::StartIedRequest* request,
    IEC61850Proto::Empty*) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求不能为空");
  }
  auto status = manager_->StartIed(request->conn_name());
  if (!status.ok()) {
    LOG_ERROR("IEC61850启动IED通信功能失败: IED={}, 原因={}",
              request->conn_name(), status.error_message());
  }
  return status;
}

grpc::Status IEC61850GrpcServiceImpl::StopIed(
    grpc::ServerContext*, const IEC61850Proto::StopIedRequest* request,
    IEC61850Proto::Empty*) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求不能为空");
  }
  auto status = manager_->StopIed(request->conn_name());
  if (!status.ok()) {
    LOG_ERROR("IEC61850停止IED通信功能失败: IED={}, 原因={}",
              request->conn_name(), status.error_message());
  }
  return status;
}

grpc::Status IEC61850GrpcServiceImpl::UpsertPointMappings(
    grpc::ServerContext*,
    const IEC61850Proto::UpsertPointMappingsRequest* request,
    IEC61850Proto::Empty*) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求不能为空");
  }
  auto status = manager_->UpsertPointMappings(*request);
  if (!status.ok()) {
    LOG_ERROR("IEC61850更新点映射失败: IED={}, 点数={}, 原因={}",
              request->conn_name(), request->points_size(),
              status.error_message());
  } else {
    LOG_INFO("IEC61850更新点映射完成: IED={}, 点数={}, 全量替换={}",
             request->conn_name(), request->points_size(), request->replace());
  }
  return status;
}

grpc::Status IEC61850GrpcServiceImpl::GetPointMappings(
    grpc::ServerContext*,
    const IEC61850Proto::GetPointMappingsRequest* request,
    IEC61850Proto::PointMappings* response) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "请求和响应不能为空");
  }
  return manager_->GetPointMappings(request->conn_name(), response);
}

grpc::Status IEC61850GrpcServiceImpl::GetRuntimeStatistics(
    grpc::ServerContext*,
    const IEC61850Proto::GetRuntimeStatisticsRequest* request,
    IEC61850Proto::RuntimeStatistics* response) {
  if (auto status = EnsureReady(); !status.ok()) {
    return status;
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "请求和响应不能为空");
  }
  return manager_->GetRuntimeStatistics(request->conn_name(), response);
}

grpc::Status IEC61850GrpcServiceImpl::EnsureReady() const {
  if (manager_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850模块尚未就绪");
  }
  return grpc::Status::OK;
}

}  // namespace IEC61850
