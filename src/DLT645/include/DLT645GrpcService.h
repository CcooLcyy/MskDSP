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
  grpc::Status UpdateConfig(grpc::ServerContext* context,
                            const DLT645Proto::UpdateConfigRequest* request,
                            DLT645Proto::UpdateConfigResponse* response) override;
  grpc::Status GetConfig(grpc::ServerContext* context,
                         const DLT645Proto::Empty* request,
                         DLT645Proto::GetConfigResponse* response) override;
  grpc::Status UpsertLink(grpc::ServerContext* context,
                          const DLT645Proto::UpsertLinkRequest* request,
                          DLT645Proto::LinkInfo* response) override;
  grpc::Status RenameLink(grpc::ServerContext* context,
                          const DLT645Proto::RenameLinkRequest* request,
                          DLT645Proto::LinkInfo* response) override;
  grpc::Status GetLink(grpc::ServerContext* context,
                       const DLT645Proto::GetLinkRequest* request,
                       DLT645Proto::LinkInfo* response) override;
  grpc::Status ListLinks(grpc::ServerContext* context,
                         const DLT645Proto::Empty* request,
                         DLT645Proto::ListLinksResponse* response) override;
  grpc::Status DeleteLink(grpc::ServerContext* context,
                          const DLT645Proto::DeleteLinkRequest* request,
                          DLT645Proto::Empty* response) override;
  grpc::Status StartLink(grpc::ServerContext* context,
                         const DLT645Proto::StartLinkRequest* request,
                         DLT645Proto::Empty* response) override;
  grpc::Status StopLink(grpc::ServerContext* context,
                        const DLT645Proto::StopLinkRequest* request,
                        DLT645Proto::Empty* response) override;
  grpc::Status UpsertPointTable(grpc::ServerContext* context,
                                const DLT645Proto::UpsertPointTableRequest* request,
                                DLT645Proto::Empty* response) override;
  grpc::Status GetPointTable(grpc::ServerContext* context,
                             const DLT645Proto::GetPointTableRequest* request,
                             DLT645Proto::PointTable* response) override;

private:
  DLT645* module_{nullptr};
};
}  // namespace DLT645
