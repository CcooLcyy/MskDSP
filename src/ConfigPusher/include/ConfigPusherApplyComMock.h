#pragma once

#include "COMMock.grpc.pb.h"
#include "ConfigPusher.pb.h"

namespace ConfigPusher {
bool applyComMockConfig(const COMMockProto::COMMockConfig &config,
                        COMMockProto::COMMockService::StubInterface *stub);
}  // namespace ConfigPusher
