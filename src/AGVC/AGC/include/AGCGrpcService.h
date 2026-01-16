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

  grpc::Status UpsertGroup(grpc::ServerContext* context, const AGCProto::UpsertGroupRequest* request, AGCProto::GroupInfo* response) override;
  grpc::Status GetGroup(grpc::ServerContext* context, const AGCProto::GetGroupRequest* request, AGCProto::GroupInfo* response) override;
  grpc::Status ListGroups(grpc::ServerContext* context, const AGCProto::Empty* request, AGCProto::ListGroupsResponse* response) override;
  grpc::Status DeleteGroup(grpc::ServerContext* context, const AGCProto::DeleteGroupRequest* request, AGCProto::Empty* response) override;
  grpc::Status StartGroup(grpc::ServerContext* context, const AGCProto::StartGroupRequest* request, AGCProto::Empty* response) override;
  grpc::Status StopGroup(grpc::ServerContext* context, const AGCProto::StopGroupRequest* request, AGCProto::Empty* response) override;

private:
  AGC* module_{nullptr};
};
}  // namespace AGC
