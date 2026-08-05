#pragma once

#include <grpcpp/support/status.h>

#include "IEC61850ProtocolStack.h"

namespace IEC61850 {

// 将SCL外部引用、点映射和A/B通信绑定编译为协议栈可直接使用的实时计划。
grpc::Status BuildRealtimeProtocolPlan(
    const IEC61850Proto::NormalizedSclModel& model,
    const IEC61850Proto::PointMappings& mappings,
    ProtocolIedPlan* plan);

}  // namespace IEC61850
