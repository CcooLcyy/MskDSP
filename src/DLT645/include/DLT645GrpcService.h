#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "DLT645.grpc.pb.h"
#include "DLT645.h"
#include "DLT645.pb.h"

namespace DLT645 {
class DLT645GrpcServiceImpl : public DLT645Proto::DLT645Service::Service {
public:
  void getDLT645(DLT645* module);
  grpc::Status Ping(grpc::ServerContext* context, const DLT645Proto::Empty*, DLT645Proto::Empty*) override;

private:
  DLT645* module_;
};
}  // namespace DLT645
