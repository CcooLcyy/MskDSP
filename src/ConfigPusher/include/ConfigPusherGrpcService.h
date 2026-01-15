#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "ConfigPusher.grpc.pb.h"
#include "ConfigPusher.h"
#include "ConfigPusher.pb.h"

namespace ConfigPusher {
class ConfigPusherGrpcServiceImpl : public ConfigPusherProto::ConfigPusherService::Service {
public:
  void getConfigPusher(ConfigPusher* module);
  grpc::Status Ping(grpc::ServerContext* context, const ConfigPusherProto::Empty*, ConfigPusherProto::Empty*) override;

private:
  ConfigPusher* module_;
};
}  // namespace ConfigPusher
