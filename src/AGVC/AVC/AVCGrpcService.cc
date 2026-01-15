#include "AVCGrpcService.h"

#include <grpcpp/support/status.h>

namespace AVC {
void AVCGrpcServiceImpl::getAVC(AVC* module) {
  module_ = module;
}
grpc::Status AVCGrpcServiceImpl::Ping(grpc::ServerContext* context, const AVCProto::Empty*, AVCProto::Empty*) {
  return grpc::Status::OK;
}
}  // namespace AVC
