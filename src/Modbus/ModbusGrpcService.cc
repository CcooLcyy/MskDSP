#include "ModbusGrpcService.h"

#include <grpcpp/support/status.h>

namespace Modbus {
void ModbusGrpcServiceImpl::getModbus(Modbus* module) {
  module_ = module;
}
grpc::Status ModbusGrpcServiceImpl::Ping(grpc::ServerContext* context, const ModbusProto::Empty*, ModbusProto::Empty*) {
  return grpc::Status::OK;
}
}  // namespace Modbus
