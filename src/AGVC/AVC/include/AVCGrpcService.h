#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "AVC.grpc.pb.h"
#include "AVC.h"
#include "AVC.pb.h"

namespace AVC {
class AVCGrpcServiceImpl : public AVCProto::AVCService::Service {
public:
  void getAVC(AVC* module);
  grpc::Status Ping(grpc::ServerContext* context, const AVCProto::Empty*, AVCProto::Empty*) override;

private:
  AVC* module_;
};
}  // namespace AVC
