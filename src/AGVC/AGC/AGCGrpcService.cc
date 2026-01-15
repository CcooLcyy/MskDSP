#include "AGCGrpcService.h"

#include <grpcpp/support/status.h>

#include "Logger.h"
#include "AGC.h"

namespace AGC {
void AGCGrpcServiceImpl::getAGC(AGC* module) {
  module_ = module;
}
grpc::Status AGCGrpcServiceImpl::Ping(grpc::ServerContext* context, const AGCProto::Empty*, AGCProto::Empty*) {
  return grpc::Status::OK;
}

grpc::Status AGCGrpcServiceImpl::UpsertGroup(
    grpc::ServerContext*, const AGCProto::UpsertGroupRequest* request, AGCProto::GroupInfo* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AGC UpsertGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request/response is null");
  }
  auto status = module_->groupManager().UpsertGroup(*request, response);
  if (!status.ok()) {
    LOG_ERROR("AGC 配置控制组失败: group_name={}, 原因={}", request->config().group_name(), status.error_message());
  } else {
    LOG_INFO("AGC 已配置控制组: group_name={}, conn_id={}", response->config().group_name(), response->conn_id());
  }
  return status;
}

grpc::Status AGCGrpcServiceImpl::GetGroup(
    grpc::ServerContext*, const AGCProto::GetGroupRequest* request, AGCProto::GroupInfo* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AGC GetGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request/response is null");
  }
  auto status = module_->groupManager().GetGroup(request->group_name(), response);
  if (!status.ok()) {
    LOG_ERROR("AGC 查询控制组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
  }
  return status;
}

grpc::Status AGCGrpcServiceImpl::ListGroups(
    grpc::ServerContext*, const AGCProto::Empty*, AGCProto::ListGroupsResponse* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (response == nullptr) {
    LOG_ERROR("AGC ListGroups 响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response is null");
  }
  auto status = module_->groupManager().ListGroups(response);
  if (!status.ok()) {
    LOG_ERROR("AGC 获取控制组列表失败: {}", status.error_message());
  }
  return status;
}

grpc::Status AGCGrpcServiceImpl::DeleteGroup(grpc::ServerContext*, const AGCProto::DeleteGroupRequest* request, AGCProto::Empty*) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr) {
    LOG_ERROR("AGC DeleteGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  auto status = module_->groupManager().DeleteGroup(request->group_name());
  if (!status.ok()) {
    LOG_ERROR("AGC 删除控制组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
  } else {
    LOG_INFO("AGC 已删除控制组: group_name={}", request->group_name());
  }
  return status;
}

grpc::Status AGCGrpcServiceImpl::StartGroup(grpc::ServerContext*, const AGCProto::StartGroupRequest* request, AGCProto::Empty*) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr) {
    LOG_ERROR("AGC StartGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  auto status = module_->groupManager().StartGroup(request->group_name());
  if (!status.ok()) {
    LOG_ERROR("AGC 启动控制组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
  } else {
    LOG_INFO("AGC 已启动控制组: group_name={}", request->group_name());
  }
  return status;
}

grpc::Status AGCGrpcServiceImpl::StopGroup(grpc::ServerContext*, const AGCProto::StopGroupRequest* request, AGCProto::Empty*) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr) {
    LOG_ERROR("AGC StopGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  auto status = module_->groupManager().StopGroup(request->group_name());
  if (!status.ok()) {
    LOG_ERROR("AGC 停止控制组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
  } else {
    LOG_INFO("AGC 已停止控制组: group_name={}", request->group_name());
  }
  return status;
}
}  // namespace AGC
