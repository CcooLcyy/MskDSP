#pragma once

#include "AGC.grpc.pb.h"
#include "ConfigPusher.pb.h"

namespace ConfigPusher {
// 下发 AGC 控制组配置，并按任务要求启动控制组内事件触发控制功能。
bool applyAgcConfig(const ConfigPusherProto::AgcConfig& config,
                    AGCProto::AGCService::StubInterface* stub);
}  // namespace ConfigPusher
