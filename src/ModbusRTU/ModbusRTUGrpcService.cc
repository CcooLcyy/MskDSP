#include "ModbusRTUGrpcService.h"

#include <grpcpp/support/status.h>

namespace ModbusRTU {
void ModbusRTUGrpcServiceImpl::setModbusRTU(ModbusRTU* module) {
  module_ = module;
}
grpc::Status ModbusRTUGrpcServiceImpl::Ping(grpc::ServerContext* context, const ModbusRTUProto::Empty*, ModbusRTUProto::Empty*) {
  return grpc::Status::OK;
}
}  // namespace ModbusRTU
