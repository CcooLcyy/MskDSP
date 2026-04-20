#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "Calc.grpc.pb.h"

namespace Calc {
class Calc;

class CalcGrpcServiceImpl : public CalcProto::CalcService::Service {
public:
  void getCalc(Calc *module);

  grpc::Status UpsertGroup(grpc::ServerContext *context, const CalcProto::UpsertGroupRequest *request, CalcProto::CalcGroupInfo *response) override;
  grpc::Status RenameGroup(grpc::ServerContext *context, const CalcProto::RenameGroupRequest *request, CalcProto::CalcGroupInfo *response) override;
  grpc::Status GetGroup(grpc::ServerContext *context, const CalcProto::GetGroupRequest *request, CalcProto::CalcGroupInfo *response) override;
  grpc::Status ListGroups(grpc::ServerContext *context, const CalcProto::Empty *request, CalcProto::ListGroupsResponse *response) override;
  grpc::Status DeleteGroup(grpc::ServerContext *context, const CalcProto::DeleteGroupRequest *request, CalcProto::Empty *response) override;
  grpc::Status StartGroup(grpc::ServerContext *context, const CalcProto::StartGroupRequest *request, CalcProto::Empty *response) override;
  grpc::Status StopGroup(grpc::ServerContext *context, const CalcProto::StopGroupRequest *request, CalcProto::Empty *response) override;

private:
  Calc *module_{nullptr};
};
}  // namespace Calc
