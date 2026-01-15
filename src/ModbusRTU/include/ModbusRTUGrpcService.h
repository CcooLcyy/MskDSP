#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "ModbusRTU.grpc.pb.h"
#include "ModbusRTU.h"
#include "ModbusRTU.pb.h"

namespace ModbusRTU {
class ModbusRTUGrpcServiceImpl : public ModbusRTUProto::ModbusRTUService::Service {
public:
  void setModbusRTU(ModbusRTU* module);
  grpc::Status Ping(grpc::ServerContext* context, const ModbusRTUProto::Empty*, ModbusRTUProto::Empty*) override;

private:
  ModbusRTU* module_;
};
}  // namespace ModbusRTU
