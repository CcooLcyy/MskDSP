#include "DLT645GrpcService.h"

#include <grpcpp/support/status.h>

namespace DLT645 {
void DLT645GrpcServiceImpl::getDLT645(DLT645* module) {
  module_ = module;
}
grpc::Status DLT645GrpcServiceImpl::Ping(grpc::ServerContext* context, const DLT645Proto::Empty*, DLT645Proto::Empty*) {
  return grpc::Status::OK;
}
}  // namespace DLT645
