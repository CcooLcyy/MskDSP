#pragma once

#include "Calc.grpc.pb.h"
#include "ConfigPusher.pb.h"

namespace ConfigPusher {
// 下发 Calc 计算分组配置；是否启动分组运算功能由模块依据当前配置自动判定。
bool applyCalcConfig(const ConfigPusherProto::CalcConfig &config,
                     CalcProto::CalcService::StubInterface *stub);
}  // namespace ConfigPusher
