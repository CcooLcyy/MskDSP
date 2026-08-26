#pragma once

#include "ConfigPusher.pb.h"
#include "ControlOrchestrator.grpc.pb.h"

namespace ConfigPusher {
bool applyControlOrchestratorConfig(
    const ConfigPusherProto::ControlOrchestratorConfig &config,
    ControlOrchestratorProto::ControlOrchestratorService::StubInterface *stub);
}  // namespace ConfigPusher
