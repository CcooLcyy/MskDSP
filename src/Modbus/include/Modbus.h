#pragma once

#include <memory>
#include <stop_token>

#include "ModuleInterface.h"

namespace Modbus {
class ModbusGrpcServiceImpl;
class Modbus : public ModuleInterface::ModuleInterface {
public:
  explicit Modbus();
  ~Modbus() override;

  void start(std::stop_token stopToken) override;

private:
  std::shared_ptr<ModbusGrpcServiceImpl> modbusService_;
};
}  // namespace Modbus
