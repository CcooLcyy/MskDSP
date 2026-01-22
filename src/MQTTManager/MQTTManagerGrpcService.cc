#include "MQTTManagerGrpcService.h"

#include <grpcpp/support/status.h>

#include "Logger.h"
#include "MQTTManager.h"

namespace MQTTManager {
void MQTTManagerGrpcServiceImpl::setMQTTManager(MQTTManager* module) {
  module_ = module;
}

grpc::Status MQTTManagerGrpcServiceImpl::Publish(
    grpc::ServerContext*, const MQTTManagerProto::PublishRequest* request, MQTTManagerProto::PublishResponse* response) {
  if (module_ == nullptr) {
    LOG_ERROR("MQTTManager 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("MQTTManager 发布请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  return module_->Publish(*request, response);
}

grpc::Status MQTTManagerGrpcServiceImpl::Subscribe(
    grpc::ServerContext*, const MQTTManagerProto::SubscribeRequest* request,
    grpc::ServerWriter<MQTTManagerProto::SubscribeResponse>* writer) {
  if (module_ == nullptr) {
    LOG_ERROR("MQTTManager 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || writer == nullptr) {
    LOG_ERROR("MQTTManager 订阅请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  return module_->Subscribe(*request, writer);
}

grpc::Status MQTTManagerGrpcServiceImpl::UpdateConfig(
    grpc::ServerContext*, const MQTTManagerProto::UpdateConfigRequest* request,
    MQTTManagerProto::UpdateConfigResponse* response) {
  if (module_ == nullptr) {
    LOG_ERROR("MQTTManager 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("MQTTManager 配置更新请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  return module_->UpdateConfig(*request, response);
}

grpc::Status MQTTManagerGrpcServiceImpl::GetStatus(
    grpc::ServerContext*, const MQTTManagerProto::GetStatusRequest* request, MQTTManagerProto::GetStatusResponse* response) {
  if (module_ == nullptr) {
    LOG_ERROR("MQTTManager 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("MQTTManager 状态查询请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  return module_->GetStatus(*request, response);
}
}  // namespace MQTTManager
