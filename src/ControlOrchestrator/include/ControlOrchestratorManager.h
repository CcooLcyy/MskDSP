#pragma once

#include <grpcpp/support/status.h>
#include <grpcpp/server_context.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "ControlOrchestrator.pb.h"
#include "ControlOrchestratorDataCenterClient.h"
#include "ControlOrchestratorStore.h"

namespace ControlOrchestrator {

class SequenceManager {
public:
  explicit SequenceManager(std::filesystem::path configDbPath = "./conf/config.db");

  void setDataCenterServerAddress(std::string address);
  void setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);

  grpc::Status LoadPersistedConfig();
  grpc::Status UpsertSequence(const ControlOrchestratorProto::WorkflowConfig &config,
                              bool createOnly,
                              ControlOrchestratorProto::WorkflowConfig *out);
  grpc::Status GetSequence(const std::string &name,
                           ControlOrchestratorProto::WorkflowConfig *out) const;
  grpc::Status ListSequences(ControlOrchestratorProto::ListSequencesResponse *out) const;
  grpc::Status DeleteSequence(const std::string &name);
  grpc::Status ExecuteSequence(const ControlOrchestratorProto::ExecuteSequenceRequest &request,
                               ControlOrchestratorProto::ExecuteSequenceResponse *response,
                               grpc::ServerContext *serverContext = nullptr);
  grpc::Status ExecuteTriggeredCommand(const DataCenterProto::ExecuteCommandRequest &request,
                                       DataCenterProto::ExecuteCommandResponse *response,
                                       grpc::ServerContext *serverContext = nullptr);

private:
  grpc::Status saveLocked();
  std::shared_ptr<std::mutex> lockForSequenceLocked(const std::string &name);

  mutable std::mutex mu_;
  std::unordered_map<std::string, ControlOrchestratorProto::WorkflowConfig> sequences_;
  std::unordered_map<std::string, std::shared_ptr<std::mutex>> sequenceLocks_;
  SequenceStore store_;
  DataCenterClient dataCenter_;
};

}  // namespace ControlOrchestrator
