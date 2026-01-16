#pragma once

#include "ConfigPusher.pb.h"
#include "DataCenter.grpc.pb.h"

namespace ConfigPusher {
bool ApplyDataCenterConfig(const ConfigPusherProto::DataCenterConfig &config,
                           DataCenterProto::DataCenterService::StubInterface *stub);
}  // namespace ConfigPusher
