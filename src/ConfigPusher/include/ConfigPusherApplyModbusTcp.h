#pragma once

#include "ConfigPusher.pb.h"
#include "ModbusTCP.grpc.pb.h"

namespace ConfigPusher {
bool applyModbusTcpConfig(const ConfigPusherProto::ModbusTcpConfig &config,
                          ModbusTCPProto::ModbusTCPService::StubInterface *stub);
}  // namespace ConfigPusher
