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
    grpc::ServerContext* context, const MQTTManagerProto::SubscribeRequest* request,
    grpc::ServerWriter<MQTTManagerProto::SubscribeResponse>* writer) {
  if (module_ == nullptr) {
    LOG_ERROR("MQTTManager 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || writer == nullptr) {
    LOG_ERROR("MQTTManager 订阅请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  return module_->Subscribe(*request, writer, context);
}

grpc::Status MQTTManagerGrpcServiceImpl::RequestAndWait(
    grpc::ServerContext*, const MQTTManagerProto::RequestAndWaitRequest* request,
    MQTTManagerProto::RequestAndWaitResponse* response) {
  if (module_ == nullptr) {
    LOG_ERROR("MQTTManager 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("MQTTManager 请求响应请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  return module_->RequestAndWait(*request, response);
}
}  // namespace MQTTManager
