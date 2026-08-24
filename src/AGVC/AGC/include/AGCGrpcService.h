#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "AGC.grpc.pb.h"
#include "AGC.h"
#include "AGC.pb.h"
#include "DataCenter.grpc.pb.h"

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
  grpc::Status StartTuning(grpc::ServerContext* context, const AGCProto::StartTuningRequest* request, AGCProto::TuningStatus* response) override;
  grpc::Status StopTuning(grpc::ServerContext* context, const AGCProto::StopTuningRequest* request, AGCProto::TuningStatus* response) override;
  grpc::Status GetTuningStatus(grpc::ServerContext* context, const AGCProto::GetTuningStatusRequest* request, AGCProto::TuningStatus* response) override;
  grpc::Status GetControlProfile(grpc::ServerContext* context, const AGCProto::GetControlProfileRequest* request, AGCProto::GroupControlProfile* response) override;
  grpc::Status ConfirmControlProfile(grpc::ServerContext* context, const AGCProto::ConfirmControlProfileRequest* request, AGCProto::GroupControlProfile* response) override;

private:
  AGC* module_{nullptr};
};

class AGCCommandExecutorServiceImpl : public DataCenterProto::CommandExecutor::Service {
public:
  void getAGC(AGC* module);

  grpc::Status ExecuteCommand(
      grpc::ServerContext* context,
      const DataCenterProto::ExecuteCommandRequest* request,
      DataCenterProto::ExecuteCommandResponse* response) override;

private:
  AGC* module_{nullptr};
};
}  // namespace AGC
