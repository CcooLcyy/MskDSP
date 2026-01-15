#include "ConfigPusherGrpcService.h"

#include <grpcpp/support/status.h>

namespace ConfigPusher {
void ConfigPusherGrpcServiceImpl::getConfigPusher(ConfigPusher* module) {
  module_ = module;
}
grpc::Status ConfigPusherGrpcServiceImpl::Ping(grpc::ServerContext* context, const ConfigPusherProto::Empty*, ConfigPusherProto::Empty*) {
  return grpc::Status::OK;
}
}  // namespace ConfigPusher
