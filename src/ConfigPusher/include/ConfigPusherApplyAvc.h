#pragma once

#include "AVC.grpc.pb.h"
#include "ConfigPusher.pb.h"

namespace ConfigPusher {
// 下发 AVC 控制组配置；是否启动控制组功能由模块依据当前配置自动判定。
bool applyAvcConfig(const ConfigPusherProto::AvcConfig& config,
                    AVCProto::AVCService::StubInterface* stub);
}  // namespace ConfigPusher
