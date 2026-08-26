#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "ControlOrchestrator.pb.h"

namespace ControlOrchestrator {

class SequenceStore {
public:
  explicit SequenceStore(std::filesystem::path configDbPath = "./conf/config.db");

  grpc::Status Save(const ControlOrchestratorProto::SequencesConfig &config);
  grpc::Status Load(ControlOrchestratorProto::SequencesConfig *out);

private:
  std::filesystem::path configDbPath_;
};

grpc::Status ValidateWorkflowConfig(const ControlOrchestratorProto::WorkflowConfig &config);
grpc::Status ValidateSequencesConfig(const ControlOrchestratorProto::SequencesConfig &config);

}  // namespace ControlOrchestrator
