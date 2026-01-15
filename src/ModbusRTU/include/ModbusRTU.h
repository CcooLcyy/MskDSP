#pragma once

#include <memory>
#include <stop_token>

#include "ModuleInterface.h"

namespace ModbusRTU {
class ModbusRTUGrpcServiceImpl;
class ModbusRTU : public ModuleInterface::ModuleInterface {
public:
  explicit ModbusRTU();
  ~ModbusRTU() override;

  void start(std::stop_token stopToken) override;

private:
  std::shared_ptr<ModbusRTUGrpcServiceImpl> modbusRTUService_;
};
}  // namespace ModbusRTU
