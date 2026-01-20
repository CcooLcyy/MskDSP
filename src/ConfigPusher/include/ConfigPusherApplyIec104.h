#pragma once

#include "ConfigPusher.pb.h"
#include "IEC104.grpc.pb.h"

namespace ConfigPusher {
bool applyIec104Config(const ConfigPusherProto::Iec104Config &config,
                       IEC104Proto::IEC104Service::StubInterface *stub);
}  // namespace ConfigPusher
