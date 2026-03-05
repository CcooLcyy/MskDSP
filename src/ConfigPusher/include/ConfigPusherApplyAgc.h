#pragma once

#include "AGC.grpc.pb.h"
#include "ConfigPusher.pb.h"

namespace ConfigPusher {
bool applyAgcConfig(const ConfigPusherProto::AgcConfig& config,
                    AGCProto::AGCService::StubInterface* stub);
}  // namespace ConfigPusher
