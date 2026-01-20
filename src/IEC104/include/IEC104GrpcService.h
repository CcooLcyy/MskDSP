#pragma once

#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "IEC104.grpc.pb.h"
#include "IEC104.h"
#include "IEC104.pb.h"

namespace IEC104 {
class IEC104GrpcServiceImpl : public IEC104Proto::IEC104Service::Service {
public:
  void getIEC104(IEC104 *iec104);
  grpc::Status UpsertLink(grpc::ServerContext *context, const IEC104Proto::UpsertLinkRequest *request, IEC104Proto::LinkInfo *response) override;
  grpc::Status GetLink(grpc::ServerContext *context, const IEC104Proto::GetLinkRequest *request, IEC104Proto::LinkInfo *response) override;
  grpc::Status ListLinks(grpc::ServerContext *context, const IEC104Proto::Empty *, IEC104Proto::ListLinksResponse *response) override;
  grpc::Status DeleteLink(grpc::ServerContext *context, const IEC104Proto::DeleteLinkRequest *request, IEC104Proto::Empty *) override;
  grpc::Status StartLink(grpc::ServerContext *context, const IEC104Proto::StartLinkRequest *request, IEC104Proto::Empty *) override;
  grpc::Status StopLink(grpc::ServerContext *context, const IEC104Proto::StopLinkRequest *request, IEC104Proto::Empty *) override;
  grpc::Status UpsertPointTable(grpc::ServerContext *context, const IEC104Proto::UpsertPointTableRequest *request, IEC104Proto::Empty *) override;
  grpc::Status GetPointTable(grpc::ServerContext *context, const IEC104Proto::GetPointTableRequest *request, IEC104Proto::PointTable *response) override;
  grpc::Status SendTimeSync(grpc::ServerContext *context, const IEC104Proto::SendTimeSyncRequest *request, IEC104Proto::Empty *) override;

private:
  IEC104 *iec104_;
};
}  // namespace IEC104
