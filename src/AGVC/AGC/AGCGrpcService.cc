#include "AGCGrpcService.h"

#include <grpcpp/support/status.h>

namespace AGC {
void AGCGrpcServiceImpl::getAGC(AGC* module) {
  module_ = module;
}
grpc::Status AGCGrpcServiceImpl::Ping(grpc::ServerContext* context, const AGCProto::Empty*, AGCProto::Empty*) {
  return grpc::Status::OK;
}
}  // namespace AGC
