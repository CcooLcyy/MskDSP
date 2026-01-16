#pragma once

#include <memory>
#include <stop_token>

#include "ModuleInterface.h"
#include "ModbusRTULinkManager.h"

namespace ModbusRTU {
class ModbusRTUGrpcServiceImpl;
class ModbusRTU : public ModuleInterface::ModuleInterface {
public:
  explicit ModbusRTU();
  ~ModbusRTU() override;

  void start(std::stop_token stopToken) override;
  LinkManager& linkManager();
  const LinkManager& linkManager() const;

private:
  std::shared_ptr<ModbusRTUGrpcServiceImpl> modbusRTUService_;
  LinkManager linkManager_;
};
}  // namespace ModbusRTU
