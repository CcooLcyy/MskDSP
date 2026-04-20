#include "CalcGrpcService.h"

#include "Calc.h"
#include "Logger.h"

namespace Calc {

void CalcGrpcServiceImpl::getCalc(Calc *module) {
  module_ = module;
}

grpc::Status CalcGrpcServiceImpl::UpsertGroup(
    grpc::ServerContext *, const CalcProto::UpsertGroupRequest *request, CalcProto::CalcGroupInfo *response) {
  if (module_ == nullptr) {
    LOG_ERROR("Calc 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("Calc UpsertGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = module_->groupManager().UpsertGroup(*request, response);
  if (!status.ok()) {
    LOG_ERROR("Calc 配置分组失败: group_name={}, 原因={}", request->config().group_name(), status.error_message());
  } else {
    LOG_INFO("Calc 已配置分组: group_name={}, conn_id={}", response->config().group_name(), response->conn_id());
  }
  return status;
}

grpc::Status CalcGrpcServiceImpl::RenameGroup(
    grpc::ServerContext *, const CalcProto::RenameGroupRequest *request, CalcProto::CalcGroupInfo *response) {
  if (module_ == nullptr) {
    LOG_ERROR("Calc 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("Calc RenameGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = module_->groupManager().RenameGroup(request->old_group_name(), request->new_group_name(), response);
  if (!status.ok()) {
    LOG_ERROR("Calc 重命名分组失败: old_group_name={}, new_group_name={}, 原因={}",
              request->old_group_name(),
              request->new_group_name(),
              status.error_message());
  } else {
    LOG_INFO("Calc 已重命名分组: old_group_name={}, new_group_name={}, conn_id={}",
             request->old_group_name(),
             response->config().group_name(),
             response->conn_id());
  }
  return status;
}

grpc::Status CalcGrpcServiceImpl::GetGroup(
    grpc::ServerContext *, const CalcProto::GetGroupRequest *request, CalcProto::CalcGroupInfo *response) {
  if (module_ == nullptr) {
    LOG_ERROR("Calc 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("Calc GetGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = module_->groupManager().GetGroup(request->group_name(), response);
  if (!status.ok()) {
    LOG_ERROR("Calc 查询分组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
  }
  return status;
}

grpc::Status CalcGrpcServiceImpl::ListGroups(
    grpc::ServerContext *, const CalcProto::Empty *, CalcProto::ListGroupsResponse *response) {
  if (module_ == nullptr) {
    LOG_ERROR("Calc 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (response == nullptr) {
    LOG_ERROR("Calc ListGroups 响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  auto status = module_->groupManager().ListGroups(response);
  if (!status.ok()) {
    LOG_ERROR("Calc 获取分组列表失败: {}", status.error_message());
  }
  return status;
}

grpc::Status CalcGrpcServiceImpl::DeleteGroup(
    grpc::ServerContext *, const CalcProto::DeleteGroupRequest *request, CalcProto::Empty *) {
  if (module_ == nullptr) {
    LOG_ERROR("Calc 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("Calc DeleteGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = module_->groupManager().DeleteGroup(request->group_name());
  if (!status.ok()) {
    LOG_ERROR("Calc 删除分组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
  } else {
    LOG_INFO("Calc 已删除分组: group_name={}", request->group_name());
  }
  return status;
}

grpc::Status CalcGrpcServiceImpl::StartGroup(
    grpc::ServerContext *, const CalcProto::StartGroupRequest *request, CalcProto::Empty *) {
  if (module_ == nullptr) {
    LOG_ERROR("Calc 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("Calc StartGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = module_->groupManager().StartGroup(request->group_name());
  if (!status.ok()) {
    LOG_ERROR("Calc 启动分组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
  } else {
    LOG_INFO("Calc 已启动分组功能: group_name={}", request->group_name());
  }
  return status;
}

grpc::Status CalcGrpcServiceImpl::StopGroup(
    grpc::ServerContext *, const CalcProto::StopGroupRequest *request, CalcProto::Empty *) {
  if (module_ == nullptr) {
    LOG_ERROR("Calc 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("Calc StopGroup 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = module_->groupManager().StopGroup(request->group_name());
  if (!status.ok()) {
    LOG_ERROR("Calc 停止分组失败: group_name={}, 原因={}", request->group_name(), status.error_message());
  } else {
    LOG_INFO("Calc 已停止分组功能: group_name={}", request->group_name());
  }
  return status;
}

}  // namespace Calc
