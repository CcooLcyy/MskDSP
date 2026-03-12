#include "ModbusRTUGrpcService.h"

#include <grpcpp/support/status.h>

#include "Logger.h"
#include "ModbusRTU.h"

namespace ModbusRTU {
void ModbusRTUGrpcServiceImpl::setModbusRTU(ModbusRTU* module) {
  module_ = module;
}
grpc::Status ModbusRTUGrpcServiceImpl::UpdateConfig(
    grpc::ServerContext*, const ModbusRTUProto::UpdateConfigRequest* request, ModbusRTUProto::UpdateConfigResponse* response) {
  if (module_ == nullptr) {
    LOG_ERROR("ModbusRTU 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("ModbusRTU UpdateConfig 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = module_->linkManager().UpdateConfig(*request, response);
  if (!status.ok()) {
    LOG_ERROR("ModbusRTU 更新 MQTT 配置失败: 原因={}", status.error_message());
  } else {
    LOG_INFO("ModbusRTU MQTT 配置更新完成: ok={}, message={}", response->ok(), response->message());
  }
  return status;
}

grpc::Status ModbusRTUGrpcServiceImpl::UpsertLink(
    grpc::ServerContext*, const ModbusRTUProto::UpsertLinkRequest* request, ModbusRTUProto::LinkInfo* response) {
  if (module_ == nullptr) {
    LOG_ERROR("ModbusRTU 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("ModbusRTU UpsertLink 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = module_->linkManager().UpsertLink(*request, response);
  if (!status.ok()) {
    LOG_ERROR("ModbusRTU 配置连接失败: conn_name={}, 原因={}", request->config().conn_name(), status.error_message());
  } else {
    LOG_INFO("ModbusRTU 已配置连接: conn_name={}, conn_id={}", request->config().conn_name(), response->conn_id());
  }
  return status;
}

grpc::Status ModbusRTUGrpcServiceImpl::GetLink(
    grpc::ServerContext*, const ModbusRTUProto::GetLinkRequest* request, ModbusRTUProto::LinkInfo* response) {
  if (module_ == nullptr) {
    LOG_ERROR("ModbusRTU 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("ModbusRTU GetLink 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = module_->linkManager().GetLink(request->conn_name(), response);
  if (!status.ok()) {
    LOG_ERROR("ModbusRTU 查询连接失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  }
  return status;
}

grpc::Status ModbusRTUGrpcServiceImpl::ListLinks(
    grpc::ServerContext*, const ModbusRTUProto::Empty*, ModbusRTUProto::ListLinksResponse* response) {
  if (module_ == nullptr) {
    LOG_ERROR("ModbusRTU 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (response == nullptr) {
    LOG_ERROR("ModbusRTU ListLinks 响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  auto status = module_->linkManager().ListLinks(response);
  if (!status.ok()) {
    LOG_ERROR("ModbusRTU 获取连接列表失败: {}", status.error_message());
  }
  return status;
}

grpc::Status ModbusRTUGrpcServiceImpl::DeleteLink(
    grpc::ServerContext*, const ModbusRTUProto::DeleteLinkRequest* request, ModbusRTUProto::Empty*) {
  if (module_ == nullptr) {
    LOG_ERROR("ModbusRTU 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("ModbusRTU DeleteLink 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = module_->linkManager().DeleteLink(request->conn_name());
  if (!status.ok()) {
    LOG_ERROR("ModbusRTU 删除连接失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  } else {
    LOG_INFO("ModbusRTU 已删除连接: conn_name={}", request->conn_name());
  }
  return status;
}

grpc::Status ModbusRTUGrpcServiceImpl::StartLink(
    grpc::ServerContext*, const ModbusRTUProto::StartLinkRequest* request, ModbusRTUProto::Empty*) {
  if (module_ == nullptr) {
    LOG_ERROR("ModbusRTU 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("ModbusRTU StartLink 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = module_->linkManager().StartLink(request->conn_name());
  if (!status.ok()) {
    LOG_ERROR("ModbusRTU 启动连接失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  } else {
    LOG_INFO("ModbusRTU 已启动连接: conn_name={}", request->conn_name());
  }
  return status;
}

grpc::Status ModbusRTUGrpcServiceImpl::StopLink(
    grpc::ServerContext*, const ModbusRTUProto::StopLinkRequest* request, ModbusRTUProto::Empty*) {
  if (module_ == nullptr) {
    LOG_ERROR("ModbusRTU 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("ModbusRTU StopLink 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = module_->linkManager().StopLink(request->conn_name());
  if (!status.ok()) {
    LOG_ERROR("ModbusRTU 停止连接失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  } else {
    LOG_INFO("ModbusRTU 已停止连接: conn_name={}", request->conn_name());
  }
  return status;
}

grpc::Status ModbusRTUGrpcServiceImpl::UpsertPointTable(
    grpc::ServerContext*, const ModbusRTUProto::UpsertPointTableRequest* request, ModbusRTUProto::Empty*) {
  if (module_ == nullptr) {
    LOG_ERROR("ModbusRTU 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("ModbusRTU UpsertPointTable 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = module_->linkManager().UpsertPointTable(*request);
  if (!status.ok()) {
    LOG_ERROR("ModbusRTU 点表更新失败: conn_name={}, 点数={}, replace={}, 原因={}",
              request->conn_name(), request->points_size(), request->replace(), status.error_message());
  } else {
    LOG_INFO("ModbusRTU 点表更新成功: conn_name={}, 点数={}, replace={}",
             request->conn_name(), request->points_size(), request->replace());
  }
  return status;
}

grpc::Status ModbusRTUGrpcServiceImpl::GetPointTable(
    grpc::ServerContext*, const ModbusRTUProto::GetPointTableRequest* request, ModbusRTUProto::PointTable* response) {
  if (module_ == nullptr) {
    LOG_ERROR("ModbusRTU 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("ModbusRTU GetPointTable 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = module_->linkManager().GetPointTable(request->conn_name(), response);
  if (!status.ok()) {
    LOG_ERROR("ModbusRTU 查询点表失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  }
  return status;
}
}  // namespace ModbusRTU
