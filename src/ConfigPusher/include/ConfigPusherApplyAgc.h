#pragma once

#include "AGC.grpc.pb.h"
#include "ConfigPusher.pb.h"

namespace ConfigPusher {
// 下发 AGC 控制组配置；是否启动控制组功能由模块依据当前配置自动判定。
bool applyAgcConfig(const ConfigPusherProto::AgcConfig& config,
                    AGCProto::AGCService::StubInterface* stub);
}  // namespace ConfigPusher
