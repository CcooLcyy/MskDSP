#include "Modbus.h"

#include <boost/dll.hpp>
#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>

#include "ModbusGrpcService.h"
#include "modbusLibInfo.h"

namespace Modbus {
Modbus::Modbus() :
  ModuleInterface(),
  modbusService_(std::make_shared<ModbusGrpcServiceImpl>()) {
  initLibInfo(modbusLibInfo);
}
Modbus::~Modbus() {}
void Modbus::start(std::stop_token stopToken) {
  modbusService_->getModbus(this);
  grpcServerBuilder(modbusService_);
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
}  // namespace Modbus

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new Modbus::Modbus();
}
