#include "IEC104GrpcService.h"

#include "Logger.h"
#include "IEC104.h"

namespace IEC104 {
namespace {
const char* roleToString(IEC104Proto::Role role) {
  switch (role) {
    case IEC104Proto::ROLE_SERVER:
      return "服务端";
    case IEC104Proto::ROLE_CLIENT:
      return "客户端";
    default:
      return "未指定";
  }
}

const char* stationRoleToString(IEC104Proto::StationRole role) {
  switch (role) {
    case IEC104Proto::STATION_ROLE_MASTER:
      return "主站";
    case IEC104Proto::STATION_ROLE_SLAVE:
      return "从站";
    default:
      return "未指定";
  }
}
}  // namespace

void IEC104GrpcServiceImpl::getIEC104(IEC104 *iec104) {
  iec104_ = iec104;
}

grpc::Status IEC104GrpcServiceImpl::UpsertLink(
    grpc::ServerContext *, const IEC104Proto::UpsertLinkRequest *request, IEC104Proto::LinkInfo *response) {
  if (iec104_ == nullptr) {
    LOG_ERROR("IEC104 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("IEC104 更新连接请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = iec104_->linkManager().UpsertLink(*request, response);
  const auto &config = status.ok() ? response->config() : request->config();
  if (!status.ok()) {
    LOG_ERROR("IEC104 配置连接失败: conn_name={}, 传输角色={}, 站点角色={}, 原因={}",
              config.conn_name(),
              roleToString(config.role()),
              stationRoleToString(config.station_role()),
              status.error_message());
  } else {
    LOG_INFO("IEC104 已配置连接: conn_name={}, 传输角色={}, 站点角色={}, conn_id={}",
             config.conn_name(),
             roleToString(config.role()),
             stationRoleToString(config.station_role()),
             response->conn_id());
  }
  return status;
}

grpc::Status IEC104GrpcServiceImpl::GetLink(
    grpc::ServerContext *, const IEC104Proto::GetLinkRequest *request, IEC104Proto::LinkInfo *response) {
  if (iec104_ == nullptr) {
    LOG_ERROR("IEC104 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("IEC104 查询连接请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = iec104_->linkManager().GetLink(request->conn_name(), response);
  if (!status.ok()) {
    LOG_ERROR("IEC104 查询连接失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  }
  return status;
}

grpc::Status IEC104GrpcServiceImpl::ListLinks(grpc::ServerContext *, const IEC104Proto::Empty *, IEC104Proto::ListLinksResponse *response) {
  if (iec104_ == nullptr) {
    LOG_ERROR("IEC104 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (response == nullptr) {
    LOG_ERROR("IEC104 连接列表响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  auto status = iec104_->linkManager().ListLinks(response);
  if (!status.ok()) {
    LOG_ERROR("IEC104 获取连接列表失败: {}", status.error_message());
  }
  return status;
}

grpc::Status IEC104GrpcServiceImpl::DeleteLink(grpc::ServerContext *, const IEC104Proto::DeleteLinkRequest *request, IEC104Proto::Empty *) {
  if (iec104_ == nullptr) {
    LOG_ERROR("IEC104 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("IEC104 删除连接请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = iec104_->linkManager().DeleteLink(request->conn_name());
  if (!status.ok()) {
    LOG_ERROR("IEC104 删除连接失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  } else {
    LOG_INFO("IEC104 已删除连接: conn_name={}", request->conn_name());
  }
  return status;
}

grpc::Status IEC104GrpcServiceImpl::StartLink(grpc::ServerContext *, const IEC104Proto::StartLinkRequest *request, IEC104Proto::Empty *) {
  if (iec104_ == nullptr) {
    LOG_ERROR("IEC104 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("IEC104 启动连接请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = iec104_->linkManager().StartLink(request->conn_name());
  if (!status.ok()) {
    LOG_ERROR("IEC104 启动连接失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  } else {
    LOG_INFO("IEC104 已启动连接: conn_name={}", request->conn_name());
  }
  return status;
}

grpc::Status IEC104GrpcServiceImpl::StopLink(grpc::ServerContext *, const IEC104Proto::StopLinkRequest *request, IEC104Proto::Empty *) {
  if (iec104_ == nullptr) {
    LOG_ERROR("IEC104 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("IEC104 停止连接请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = iec104_->linkManager().StopLink(request->conn_name());
  if (!status.ok()) {
    LOG_ERROR("IEC104 停止连接失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  } else {
    LOG_INFO("IEC104 已停止连接: conn_name={}", request->conn_name());
  }
  return status;
}

grpc::Status IEC104GrpcServiceImpl::UpsertPointTable(
    grpc::ServerContext *, const IEC104Proto::UpsertPointTableRequest *request, IEC104Proto::Empty *) {
  if (iec104_ == nullptr) {
    LOG_ERROR("IEC104 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("IEC104 点表更新请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = iec104_->linkManager().UpsertPointTable(*request);
  if (!status.ok()) {
    LOG_ERROR("IEC104 点表更新失败: conn_name={}, 点数={}, replace={}, 原因={}",
              request->conn_name(), request->points_size(), request->replace(), status.error_message());
  } else {
    LOG_INFO("IEC104 点表更新成功: conn_name={}, 点数={}, replace={}",
             request->conn_name(), request->points_size(), request->replace());
  }
  return status;
}

grpc::Status IEC104GrpcServiceImpl::GetPointTable(
    grpc::ServerContext *, const IEC104Proto::GetPointTableRequest *request, IEC104Proto::PointTable *response) {
  if (iec104_ == nullptr) {
    LOG_ERROR("IEC104 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("IEC104 点表查询请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = iec104_->linkManager().GetPointTable(request->conn_name(), response);
  if (!status.ok()) {
    LOG_ERROR("IEC104 查询点表失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  }
  return status;
}

grpc::Status IEC104GrpcServiceImpl::SendTimeSync(
    grpc::ServerContext *, const IEC104Proto::SendTimeSyncRequest *request, IEC104Proto::Empty *) {
  if (iec104_ == nullptr) {
    LOG_ERROR("IEC104 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("IEC104 对时请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = iec104_->linkManager().SendTimeSync(request->conn_name(), request->ts_ms());
  if (!status.ok()) {
    LOG_ERROR("IEC104 对时发送失败: conn_name={}, ts_ms={}, 原因={}",
              request->conn_name(),
              request->ts_ms(),
              status.error_message());
  } else {
    LOG_INFO("IEC104 已触发对时发送: conn_name={}, ts_ms={}", request->conn_name(), request->ts_ms());
  }
  return status;
}
}  // namespace IEC104
