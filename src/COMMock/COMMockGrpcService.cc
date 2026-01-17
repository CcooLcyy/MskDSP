#include "COMMockGrpcService.h"

#include "Logger.h"

namespace COMMock {
void COMMockGrpcServiceImpl::getCOMMock(COMMock *module) {
  module_ = module;
}

grpc::Status COMMockGrpcServiceImpl::ApplyConfig(grpc::ServerContext *, const COMMockProto::COMMockConfig *request,
                                                 COMMockProto::Empty *) {
  if (module_ == nullptr) {
    LOG_ERROR("COMMock 服务未绑定模块实例");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模块未就绪");
  }
  if (request == nullptr) {
    LOG_ERROR("COMMock ApplyConfig 请求为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "config 不能为空");
  }
  return module_->ApplyConfig(*request);
}
}  // namespace COMMock
