#pragma once
#include <memory>
#include <stop_token>
#include "ModuleInterface.h"
#include "ModbusTCPLinkManager.h"
namespace ModbusTCP {
class GrpcService;
class CommandService;
class ModbusTCP : public ModuleInterface::ModuleInterface {
public:
  ModbusTCP();
  ~ModbusTCP() override;
  void start(std::stop_token stopToken) override;
  LinkManager& linkManager();
private:
  std::shared_ptr<GrpcService> service_;
  std::shared_ptr<CommandService> commandService_;
  LinkManager manager_;
};
}
