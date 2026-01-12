#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "Modbus.grpc.pb.h"
#include "Modbus.h"
#include "Modbus.pb.h"

namespace Modbus {
class ModbusGrpcServiceImpl : public ModbusProto::ModbusService::Service {
public:
  void getModbus(Modbus* module);
  grpc::Status Ping(grpc::ServerContext* context, const ModbusProto::Empty*, ModbusProto::Empty*) override;

private:
  Modbus* module_;
};
}  // namespace Modbus
