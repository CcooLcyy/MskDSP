#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "AGC.grpc.pb.h"
#include "AGC.h"
#include "AGC.pb.h"

namespace AGC {
class AGCGrpcServiceImpl : public AGCProto::AGCService::Service {
public:
  void getAGC(AGC* module);
  grpc::Status Ping(grpc::ServerContext* context, const AGCProto::Empty*, AGCProto::Empty*) override;

private:
  AGC* module_;
};
}  // namespace AGC
