#pragma once

#include "OrdinaryNetworkConfig.h"

namespace mskdsp::platformagent {
// 执行参数化的独立网络事务；失败时返回中文错误，不通过 shell 执行配置值。
OrdinaryNetworkResult ApplyOrdinaryNetwork(const OrdinaryNetworkConfig &config);
}  // namespace mskdsp::platformagent
