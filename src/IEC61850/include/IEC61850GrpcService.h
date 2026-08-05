#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "IEC61850.grpc.pb.h"
#include "IEC61850.pb.h"

namespace IEC61850 {
class Manager;

class IEC61850GrpcServiceImpl : public IEC61850Proto::IEC61850Service::Service {
public:
  void SetManager(Manager* manager);

  grpc::Status ApplyTargetConfig(grpc::ServerContext*, const IEC61850Proto::ApplyTargetConfigRequest*, IEC61850Proto::ApplyTargetConfigResponse*) override;
  grpc::Status ImportScl(grpc::ServerContext*, const IEC61850Proto::ImportSclRequest*, IEC61850Proto::ImportSclResponse*) override;
  grpc::Status GetModelSummary(grpc::ServerContext*, const IEC61850Proto::GetModelSummaryRequest*, IEC61850Proto::SclModelSummary*) override;
  grpc::Status ListModels(grpc::ServerContext*, const IEC61850Proto::Empty*, IEC61850Proto::ListModelsResponse*) override;
  grpc::Status DeleteModel(grpc::ServerContext*, const IEC61850Proto::DeleteModelRequest*, IEC61850Proto::Empty*) override;
  grpc::Status UpsertIed(grpc::ServerContext*, const IEC61850Proto::UpsertIedRequest*, IEC61850Proto::IedInfo*) override;
  grpc::Status GetIed(grpc::ServerContext*, const IEC61850Proto::GetIedRequest*, IEC61850Proto::IedInfo*) override;
  grpc::Status ListIeds(grpc::ServerContext*, const IEC61850Proto::Empty*, IEC61850Proto::ListIedsResponse*) override;
  grpc::Status DeleteIed(grpc::ServerContext*, const IEC61850Proto::DeleteIedRequest*, IEC61850Proto::Empty*) override;
  grpc::Status StartIed(grpc::ServerContext*, const IEC61850Proto::StartIedRequest*, IEC61850Proto::Empty*) override;
  grpc::Status StopIed(grpc::ServerContext*, const IEC61850Proto::StopIedRequest*, IEC61850Proto::Empty*) override;
  grpc::Status UpsertPointMappings(grpc::ServerContext*, const IEC61850Proto::UpsertPointMappingsRequest*, IEC61850Proto::Empty*) override;
  grpc::Status GetPointMappings(grpc::ServerContext*, const IEC61850Proto::GetPointMappingsRequest*, IEC61850Proto::PointMappings*) override;
  grpc::Status GetRuntimeStatistics(grpc::ServerContext*, const IEC61850Proto::GetRuntimeStatisticsRequest*, IEC61850Proto::RuntimeStatistics*) override;

private:
  grpc::Status EnsureReady() const;

  Manager* manager_{nullptr};
};
}  // namespace IEC61850
