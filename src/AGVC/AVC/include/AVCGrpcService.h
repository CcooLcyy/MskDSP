#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "AVC.grpc.pb.h"
#include "AVC.h"
#include "AVC.pb.h"
#include "DataCenter.grpc.pb.h"

namespace AVC {
class AVCGrpcServiceImpl : public AVCProto::AVCService::Service {
public:
  void getAVC(AVC* module);

  grpc::Status UpsertGroup(grpc::ServerContext* context, const AVCProto::UpsertGroupRequest* request, AVCProto::GroupInfo* response) override;
  grpc::Status RenameGroup(grpc::ServerContext* context, const AVCProto::RenameGroupRequest* request, AVCProto::GroupInfo* response) override;
  grpc::Status GetGroup(grpc::ServerContext* context, const AVCProto::GetGroupRequest* request, AVCProto::GroupInfo* response) override;
  grpc::Status ListGroups(grpc::ServerContext* context, const AVCProto::Empty* request, AVCProto::ListGroupsResponse* response) override;
  grpc::Status DeleteGroup(grpc::ServerContext* context, const AVCProto::DeleteGroupRequest* request, AVCProto::Empty* response) override;
  grpc::Status StartGroup(grpc::ServerContext* context, const AVCProto::StartGroupRequest* request, AVCProto::Empty* response) override;
  grpc::Status StopGroup(grpc::ServerContext* context, const AVCProto::StopGroupRequest* request, AVCProto::Empty* response) override;

private:
  AVC* module_{nullptr};
};

class AVCCommandExecutorServiceImpl : public DataCenterProto::CommandExecutor::Service {
public:
  void getAVC(AVC* module);

  grpc::Status ExecuteCommand(
      grpc::ServerContext* context,
      const DataCenterProto::ExecuteCommandRequest* request,
      DataCenterProto::ExecuteCommandResponse* response) override;

private:
  AVC* module_{nullptr};
};
}  // namespace AVC
