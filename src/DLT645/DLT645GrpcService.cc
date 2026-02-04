#include "DLT645GrpcService.h"

#include <grpcpp/support/status.h>

#include <google/protobuf/message.h>

#include "Logger.h"

namespace {
std::string formatProtoForLog(const google::protobuf::Message& message) {
  auto text = message.ShortDebugString();
  if (text.empty()) {
    return "空";
  }
  return text;
}
}  // namespace

namespace DLT645 {
void DLT645GrpcServiceImpl::getDLT645(DLT645* module) {
  module_ = module;
}

grpc::Status DLT645GrpcServiceImpl::UpdateConfig(grpc::ServerContext*,
                                                 const DLT645Proto::UpdateConfigRequest* request,
                                                 DLT645Proto::UpdateConfigResponse* response) {
  if (module_ == nullptr) {
    LOG_ERROR("DLT645 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DLT645 UpdateConfig 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  LOG_INFO("收到 DLT645 配置更新请求报文: {}", formatProtoForLog(*request));
  auto status = module_->linkManager().UpdateConfig(*request, response);
  LOG_INFO("收到 DLT645 配置更新响应报文: {}", formatProtoForLog(*response));
  if (!status.ok()) {
    LOG_ERROR("DLT645 配置更新失败: 原因={}", status.error_message());
  }
  return status;
}

grpc::Status DLT645GrpcServiceImpl::UpsertLink(grpc::ServerContext*,
                                               const DLT645Proto::UpsertLinkRequest* request,
                                               DLT645Proto::LinkInfo* response) {
  if (module_ == nullptr) {
    LOG_ERROR("DLT645 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DLT645 UpsertLink 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  LOG_INFO("收到 DLT645 连接配置请求报文: {}", formatProtoForLog(*request));
  auto status = module_->linkManager().UpsertLink(*request, response);
  LOG_INFO("收到 DLT645 连接配置响应报文: {}", formatProtoForLog(*response));
  if (!status.ok()) {
    LOG_ERROR("DLT645 连接配置失败: conn_name={}, 原因={}", request->config().conn_name(), status.error_message());
  }
  return status;
}

grpc::Status DLT645GrpcServiceImpl::GetLink(grpc::ServerContext*,
                                            const DLT645Proto::GetLinkRequest* request,
                                            DLT645Proto::LinkInfo* response) {
  if (module_ == nullptr) {
    LOG_ERROR("DLT645 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DLT645 GetLink 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = module_->linkManager().GetLink(request->conn_name(), response);
  if (!status.ok()) {
    LOG_ERROR("DLT645 查询连接失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  }
  return status;
}

grpc::Status DLT645GrpcServiceImpl::ListLinks(grpc::ServerContext*,
                                              const DLT645Proto::Empty*,
                                              DLT645Proto::ListLinksResponse* response) {
  if (module_ == nullptr) {
    LOG_ERROR("DLT645 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (response == nullptr) {
    LOG_ERROR("DLT645 ListLinks 响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  auto status = module_->linkManager().ListLinks(response);
  if (!status.ok()) {
    LOG_ERROR("DLT645 获取连接列表失败: {}", status.error_message());
  }
  return status;
}

grpc::Status DLT645GrpcServiceImpl::DeleteLink(grpc::ServerContext*,
                                               const DLT645Proto::DeleteLinkRequest* request,
                                               DLT645Proto::Empty*) {
  if (module_ == nullptr) {
    LOG_ERROR("DLT645 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("DLT645 DeleteLink 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = module_->linkManager().DeleteLink(request->conn_name());
  if (!status.ok()) {
    LOG_ERROR("DLT645 删除连接失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  } else {
    LOG_INFO("DLT645 已删除连接: conn_name={}", request->conn_name());
  }
  return status;
}

grpc::Status DLT645GrpcServiceImpl::StartLink(grpc::ServerContext*,
                                              const DLT645Proto::StartLinkRequest* request,
                                              DLT645Proto::Empty*) {
  if (module_ == nullptr) {
    LOG_ERROR("DLT645 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("DLT645 StartLink 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = module_->linkManager().StartLink(request->conn_name());
  if (!status.ok()) {
    LOG_ERROR("DLT645 启动连接失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  } else {
    LOG_INFO("DLT645 已启动连接: conn_name={}", request->conn_name());
  }
  return status;
}

grpc::Status DLT645GrpcServiceImpl::StopLink(grpc::ServerContext*,
                                             const DLT645Proto::StopLinkRequest* request,
                                             DLT645Proto::Empty*) {
  if (module_ == nullptr) {
    LOG_ERROR("DLT645 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("DLT645 StopLink 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = module_->linkManager().StopLink(request->conn_name());
  if (!status.ok()) {
    LOG_ERROR("DLT645 停止连接失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  } else {
    LOG_INFO("DLT645 已停止连接: conn_name={}", request->conn_name());
  }
  return status;
}

grpc::Status DLT645GrpcServiceImpl::UpsertPointTable(grpc::ServerContext*,
                                                     const DLT645Proto::UpsertPointTableRequest* request,
                                                     DLT645Proto::Empty*) {
  if (module_ == nullptr) {
    LOG_ERROR("DLT645 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("DLT645 UpsertPointTable 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求为空");
  }
  auto status = module_->linkManager().UpsertPointTable(*request);
  if (!status.ok()) {
    LOG_ERROR("DLT645 点表更新失败: conn_name={}, 点数={}, 数据块数={}, replace={}, 原因={}",
              request->conn_name(), request->points_size(), request->blocks_size(),
              request->replace(), status.error_message());
  } else {
    LOG_INFO("DLT645 点表更新成功: conn_name={}, 点数={}, 数据块数={}, replace={}",
             request->conn_name(), request->points_size(), request->blocks_size(),
             request->replace());
  }
  return status;
}

grpc::Status DLT645GrpcServiceImpl::GetPointTable(grpc::ServerContext*,
                                                  const DLT645Proto::GetPointTableRequest* request,
                                                  DLT645Proto::PointTable* response) {
  if (module_ == nullptr) {
    LOG_ERROR("DLT645 服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("DLT645 GetPointTable 请求/响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "请求/响应为空");
  }
  auto status = module_->linkManager().GetPointTable(request->conn_name(), response);
  if (!status.ok()) {
    LOG_ERROR("DLT645 查询点表失败: conn_name={}, 原因={}", request->conn_name(), status.error_message());
  }
  return status;
}
}  // namespace DLT645
