#include "AGCGrpcService.h"

#include <grpcpp/support/status.h>

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
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request/response is null");
  }
  return module_->groupManager().UpsertGroup(*request, response);
}

grpc::Status AGCGrpcServiceImpl::GetGroup(
    grpc::ServerContext*, const AGCProto::GetGroupRequest* request, AGCProto::GroupInfo* response) {
  if (module_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request/response is null");
  }
  return module_->groupManager().GetGroup(request->group_name(), response);
}

grpc::Status AGCGrpcServiceImpl::ListGroups(
    grpc::ServerContext*, const AGCProto::Empty*, AGCProto::ListGroupsResponse* response) {
  if (module_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response is null");
  }
  return module_->groupManager().ListGroups(response);
}

grpc::Status AGCGrpcServiceImpl::DeleteGroup(grpc::ServerContext*, const AGCProto::DeleteGroupRequest* request, AGCProto::Empty*) {
  if (module_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  return module_->groupManager().DeleteGroup(request->group_name());
}

grpc::Status AGCGrpcServiceImpl::StartGroup(grpc::ServerContext*, const AGCProto::StartGroupRequest* request, AGCProto::Empty*) {
  if (module_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  return module_->groupManager().StartGroup(request->group_name());
}

grpc::Status AGCGrpcServiceImpl::StopGroup(grpc::ServerContext*, const AGCProto::StopGroupRequest* request, AGCProto::Empty*) {
  if (module_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "module not ready");
  }
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "request is null");
  }
  return module_->groupManager().StopGroup(request->group_name());
}
}  // namespace AGC
