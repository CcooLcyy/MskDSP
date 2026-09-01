#pragma once

#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "ControlOrchestrator.grpc.pb.h"
#include "DataCenter.grpc.pb.h"

namespace ControlOrchestrator {
class SequenceManager;

class GrpcServiceImpl : public ControlOrchestratorProto::ControlOrchestratorService::Service {
public:
  void setManager(SequenceManager *manager);

  grpc::Status UpsertSequence(grpc::ServerContext *context,
                               const ControlOrchestratorProto::UpsertSequenceRequest *request,
                               ControlOrchestratorProto::WorkflowConfig *response) override;
  grpc::Status GetSequence(grpc::ServerContext *context,
                           const ControlOrchestratorProto::GetSequenceRequest *request,
                           ControlOrchestratorProto::WorkflowConfig *response) override;
  grpc::Status ListSequences(grpc::ServerContext *context,
                             const ControlOrchestratorProto::ListSequencesRequest *request,
                             ControlOrchestratorProto::ListSequencesResponse *response) override;
  grpc::Status DeleteSequence(grpc::ServerContext *context,
                              const ControlOrchestratorProto::DeleteSequenceRequest *request,
                              DataCenterProto::Empty *response) override;
  grpc::Status ExecuteSequence(grpc::ServerContext *context,
                               const ControlOrchestratorProto::ExecuteSequenceRequest *request,
                               ControlOrchestratorProto::ExecuteSequenceResponse *response) override;

private:
  SequenceManager *manager_{nullptr};
};
}  // namespace ControlOrchestrator

namespace ControlOrchestrator {

class CommandExecutorGrpcServiceImpl : public DataCenterProto::CommandExecutor::Service {
public:
  void setManager(SequenceManager *manager);

  grpc::Status ExecuteCommand(grpc::ServerContext *context,
                              const DataCenterProto::ExecuteCommandRequest *request,
                              DataCenterProto::ExecuteCommandResponse *response) override;

private:
  SequenceManager *manager_{nullptr};
};

}  // namespace ControlOrchestrator
