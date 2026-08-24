#include "AGCGrpcService.h"

#include <grpcpp/support/status.h>

#include "Logger.h"
#include "AGC.h"

namespace AGC {
void AGCGrpcServiceImpl::getAGC(AGC* module) {
  module_ = module;
}

grpc::Status AGCGrpcServiceImpl::UpsertGroup(
    grpc::ServerContext*, const AGCProto::UpsertGroupRequest* request, AGCProto::GroupInfo* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AGC UpsertGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
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
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AGC GetGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
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
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (response == nullptr) {
    LOG_ERROR("AGC ListGroups 响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
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
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("AGC DeleteGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
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
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("AGC StartGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
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
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("AGC StopGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = module_->groupManager().StopGroup(request->group_name());
  if (!status.ok()) {
    LOG_ERROR("AGC 停止控制组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
  } else {
    LOG_INFO("AGC 已停止控制组: group_name={}", request->group_name());
  }
  return status;
}

grpc::Status AGCGrpcServiceImpl::StartTuning(
    grpc::ServerContext*, const AGCProto::StartTuningRequest* request, AGCProto::TuningStatus* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 调试服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AGC StartTuning 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = module_->groupManager().StartTuning(*request, response);
  if (!status.ok()) {
    LOG_ERROR("AGC 启动自动调试失败: group_name={}, 原因={}", request->group_name(), status.error_message());
  } else {
    LOG_INFO("AGC 启动自动调试请求已接受: group_name={}, state={}", request->group_name(), response->state());
  }
  return status;
}

grpc::Status AGCGrpcServiceImpl::StopTuning(
    grpc::ServerContext*, const AGCProto::StopTuningRequest* request, AGCProto::TuningStatus* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 调试服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AGC StopTuning 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = module_->groupManager().StopTuning(request->group_name(), response);
  if (!status.ok()) {
    LOG_ERROR("AGC 停止自动调试失败: group_name={}, 原因={}", request->group_name(), status.error_message());
  }
  return status;
}

grpc::Status AGCGrpcServiceImpl::GetTuningStatus(
    grpc::ServerContext*, const AGCProto::GetTuningStatusRequest* request, AGCProto::TuningStatus* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 调试服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AGC GetTuningStatus 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  return module_->groupManager().GetTuningStatus(request->group_name(), response);
}

grpc::Status AGCGrpcServiceImpl::GetControlProfile(
    grpc::ServerContext*, const AGCProto::GetControlProfileRequest* request, AGCProto::GroupControlProfile* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 固定控制参数服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AGC GetControlProfile 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  return module_->groupManager().GetControlProfile(request->group_name(), response);
}

grpc::Status AGCGrpcServiceImpl::ConfirmControlProfile(
    grpc::ServerContext*, const AGCProto::ConfirmControlProfileRequest* request, AGCProto::GroupControlProfile* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 固定控制参数服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr || !request->has_profile()) {
    LOG_ERROR("AGC ConfirmControlProfile 请求/响应为空或缺少 profile");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空或缺少 profile");
  }
  auto status = module_->groupManager().ConfirmControlProfile(request->profile(), response);
  if (!status.ok()) {
    LOG_ERROR("AGC 确认固定控制参数失败: group_name={}, 原因={}", request->profile().group_name(), status.error_message());
  } else {
    LOG_INFO("AGC 已确认固定控制参数: group_name={}, version={}", response->group_name(), response->version());
  }
  return status;
}

void AGCCommandExecutorServiceImpl::getAGC(AGC* module) {
  module_ = module;
}

grpc::Status AGCCommandExecutorServiceImpl::ExecuteCommand(
    grpc::ServerContext*,
    const DataCenterProto::ExecuteCommandRequest* request,
    DataCenterProto::ExecuteCommandResponse* response) {
  if (module_ == nullptr) {
    LOG_ERROR("AGC 同步命令服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("AGC 同步命令请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = module_->groupManager().ExecuteCommand(*request, response);
  if (!status.ok()) {
    LOG_ERROR("AGC 同步命令执行失败: dst_tag={}, 原因={}", request->dst().tag(), status.error_message());
  }
  return status;
}
}  // namespace AGC
