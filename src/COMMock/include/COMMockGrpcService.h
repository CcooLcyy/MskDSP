#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "COMMock.grpc.pb.h"
#include "COMMock.h"
#include "COMMock.pb.h"

namespace COMMock {
class COMMockGrpcServiceImpl : public COMMockProto::COMMockService::Service {
public:
  void getCOMMock(COMMock *module);
  grpc::Status ApplyConfig(grpc::ServerContext *context, const COMMockProto::COMMockConfig *request,
                           COMMockProto::Empty *response) override;

private:
  COMMock *module_ = nullptr;
};
}  // namespace COMMock
