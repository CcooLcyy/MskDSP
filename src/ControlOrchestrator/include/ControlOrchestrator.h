#pragma once

#include <memory>
#include <stop_token>

#include "ControlOrchestratorManager.h"
#include "ModuleInterface.h"

namespace ControlOrchestrator {
class GrpcServiceImpl;
class CommandExecutorGrpcServiceImpl;

class ControlOrchestrator : public ModuleInterface::ModuleInterface {
public:
  ControlOrchestrator();
  ~ControlOrchestrator() override;
  void start(std::stop_token stopToken) override;
  SequenceManager &sequenceManager();

private:
  std::shared_ptr<GrpcServiceImpl> service_;
  std::shared_ptr<CommandExecutorGrpcServiceImpl> commandService_;
  SequenceManager manager_;
};
}  // namespace ControlOrchestrator
