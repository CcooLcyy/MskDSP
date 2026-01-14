#include "IEC104GrpcService.h"

#include "IEC104.h"

namespace IEC104 {
void IEC104GrpcServiceImpl::getIEC104(IEC104 *iec104) {
  iec104_ = iec104;
}
grpc::Status IEC104GrpcServiceImpl::Ping(grpc::ServerContext *, const IEC104Proto::Empty *, IEC104Proto::Empty *) {
  return grpc::Status::OK;
}
grpc::Status IEC104GrpcServiceImpl::Test(grpc::ServerContext *context, const IEC104Proto::Empty *, IEC104Proto::Empty *) {
  return grpc::Status::OK;
}

grpc::Status IEC104GrpcServiceImpl::UpsertLink(
    grpc::ServerContext *, const IEC104Proto::UpsertLinkRequest *request, IEC104Proto::LinkInfo *response) {
  if (iec104_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request/response is null");
  }
  return iec104_->linkManager().UpsertLink(*request, response);
}

grpc::Status IEC104GrpcServiceImpl::GetLink(
    grpc::ServerContext *, const IEC104Proto::GetLinkRequest *request, IEC104Proto::LinkInfo *response) {
  if (iec104_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request/response is null");
  }
  return iec104_->linkManager().GetLink(request->conn_name(), response);
}

grpc::Status IEC104GrpcServiceImpl::ListLinks(grpc::ServerContext *, const IEC104Proto::Empty *, IEC104Proto::ListLinksResponse *response) {
  if (iec104_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response is null");
  }
  return iec104_->linkManager().ListLinks(response);
}

grpc::Status IEC104GrpcServiceImpl::DeleteLink(grpc::ServerContext *, const IEC104Proto::DeleteLinkRequest *request, IEC104Proto::Empty *) {
  if (iec104_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  return iec104_->linkManager().DeleteLink(request->conn_name());
}

grpc::Status IEC104GrpcServiceImpl::StartLink(grpc::ServerContext *, const IEC104Proto::StartLinkRequest *request, IEC104Proto::Empty *) {
  if (iec104_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  return iec104_->linkManager().StartLink(request->conn_name());
}

grpc::Status IEC104GrpcServiceImpl::StopLink(grpc::ServerContext *, const IEC104Proto::StopLinkRequest *request, IEC104Proto::Empty *) {
  if (iec104_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  return iec104_->linkManager().StopLink(request->conn_name());
}

grpc::Status IEC104GrpcServiceImpl::UpsertPointTable(
    grpc::ServerContext *, const IEC104Proto::UpsertPointTableRequest *request, IEC104Proto::Empty *) {
  if (iec104_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  return iec104_->linkManager().UpsertPointTable(*request);
}

grpc::Status IEC104GrpcServiceImpl::GetPointTable(
    grpc::ServerContext *, const IEC104Proto::GetPointTableRequest *request, IEC104Proto::PointTable *response) {
  if (iec104_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request/response is null");
  }
  return iec104_->linkManager().GetPointTable(request->conn_name(), response);
}
}  // namespace IEC104
