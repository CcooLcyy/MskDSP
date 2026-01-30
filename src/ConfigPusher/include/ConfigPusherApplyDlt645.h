#pragma once

#include "ConfigPusher.pb.h"
#include "DLT645.grpc.pb.h"

namespace ConfigPusher {
bool applyDlt645Config(const ConfigPusherProto::Dlt645Config& config,
                       DLT645Proto::DLT645Service::StubInterface* stub);
}  // namespace ConfigPusher
