#pragma once

#include "ConfigPusher.pb.h"
#include "ModbusRTU.grpc.pb.h"

namespace ConfigPusher {
bool applyModbusRtuConfig(const ConfigPusherProto::ModbusRtuConfig &config,
                          ModbusRTUProto::ModbusRTUService::StubInterface *stub);
}  // namespace ConfigPusher
