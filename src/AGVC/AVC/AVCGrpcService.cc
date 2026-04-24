#include "AVCGrpcService.h"

#include <grpcpp/support/status.h>

#include "Logger.h"
#include "ProtoLogUtil.hpp"

namespace AVC {
void AVCGrpcServiceImpl::getAVC(AVC* module) {
  module_ = module;
}

grpc::Status AVCGrpcServiceImpl::UpsertGroup(
    grpc::ServerContext*, const AVCProto::UpsertGroupRequest* request, AVCProto::GroupInfo* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AVC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AVC UpsertGroup 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  LOG_INFO("收到 AVC 控制组配置请求报文: {}", formatProtoForLog(*request));
  auto status = module_->groupManager().UpsertGroup(*request, response);
  if (!status.ok()) {
    LOG_ERROR("AVC 配置控制组失败: group_name={}, 原因={}", request->config().group_name(), status.error_message());
    return status;
  }
  LOG_INFO("收到 AVC 控制组配置响应报文: {}", formatProtoForLog(*response));
  LOG_INFO("AVC 已配置控制组: group_name={}, conn_id={}", response->config().group_name(), response->conn_id());
  return grpc::Status::OK;
}

grpc::Status AVCGrpcServiceImpl::RenameGroup(
    grpc::ServerContext*, const AVCProto::RenameGroupRequest* request, AVCProto::GroupInfo* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AVC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AVC RenameGroup 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  LOG_INFO("收到 AVC 控制组改名请求报文: {}", formatProtoForLog(*request));
  auto status = module_->groupManager().RenameGroup(request->old_group_name(), request->new_group_name(), response);
  if (!status.ok()) {
    LOG_ERROR("AVC 控制组改名失败: old_group_name={}, new_group_name={}, 原因={}",
              request->old_group_name(),
              request->new_group_name(),
              status.error_message());
    return status;
  }
  LOG_INFO("收到 AVC 控制组改名响应报文: {}", formatProtoForLog(*response));
  return grpc::Status::OK;
}

grpc::Status AVCGrpcServiceImpl::GetGroup(
    grpc::ServerContext*, const AVCProto::GetGroupRequest* request, AVCProto::GroupInfo* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AVC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AVC GetGroup 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  LOG_INFO("收到 AVC 查询控制组请求报文: {}", formatProtoForLog(*request));
  auto status = module_->groupManager().GetGroup(request->group_name(), response);
  if (!status.ok()) {
    LOG_ERROR("AVC 查询控制组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
    return status;
  }
  LOG_INFO("收到 AVC 查询控制组响应报文: {}", formatProtoForLog(*response));
  return grpc::Status::OK;
}

grpc::Status AVCGrpcServiceImpl::ListGroups(
    grpc::ServerContext*, const AVCProto::Empty* request, AVCProto::ListGroupsResponse* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AVC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AVC ListGroups 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  LOG_INFO("收到 AVC 控制组列表请求报文: {}", formatProtoForLog(*request));
  auto status = module_->groupManager().ListGroups(response);
  if (!status.ok()) {
    LOG_ERROR("AVC 获取控制组列表失败: {}", status.error_message());
    return status;
  }
  LOG_INFO("收到 AVC 控制组列表响应报文: {}", formatProtoForLog(*response));
  return grpc::Status::OK;
}

grpc::Status AVCGrpcServiceImpl::DeleteGroup(
    grpc::ServerContext*, const AVCProto::DeleteGroupRequest* request, AVCProto::Empty* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AVC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AVC DeleteGroup 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  LOG_INFO("收到 AVC 删除控制组请求报文: {}", formatProtoForLog(*request));
  auto status = module_->groupManager().DeleteGroup(request->group_name());
  if (!status.ok()) {
    LOG_ERROR("AVC 删除控制组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
    return status;
  }
  LOG_INFO("收到 AVC 删除控制组响应报文: {}", formatProtoForLog(*response));
  LOG_INFO("AVC 已删除控制组: group_name={}", request->group_name());
  return grpc::Status::OK;
}

grpc::Status AVCGrpcServiceImpl::StartGroup(
    grpc::ServerContext*, const AVCProto::StartGroupRequest* request, AVCProto::Empty* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AVC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AVC StartGroup 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  LOG_INFO("收到 AVC 启动控制组请求报文: {}", formatProtoForLog(*request));
  auto status = module_->groupManager().StartGroup(request->group_name());
  if (!status.ok()) {
    LOG_ERROR("AVC 启动控制组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
    return status;
  }
  LOG_INFO("收到 AVC 启动控制组响应报文: {}", formatProtoForLog(*response));
  LOG_INFO("AVC 已启动控制组: group_name={}", request->group_name());
  return grpc::Status::OK;
}

grpc::Status AVCGrpcServiceImpl::StopGroup(
    grpc::ServerContext*, const AVCProto::StopGroupRequest* request, AVCProto::Empty* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AVC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AVC StopGroup 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  LOG_INFO("收到 AVC 停止控制组请求报文: {}", formatProtoForLog(*request));
  auto status = module_->groupManager().StopGroup(request->group_name());
  if (!status.ok()) {
    LOG_ERROR("AVC 停止控制组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
    return status;
  }
  LOG_INFO("收到 AVC 停止控制组响应报文: {}", formatProtoForLog(*response));
  LOG_INFO("AVC 已停止控制组: group_name={}", request->group_name());
  return grpc::Status::OK;
}
}  // namespace AVC
