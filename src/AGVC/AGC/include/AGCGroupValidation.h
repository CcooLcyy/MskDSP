#pragma once

#include <grpcpp/support/status.h>

#include "AGC.pb.h"

namespace AGC {

grpc::Status ValidateGroupConfig(const AGCProto::GroupConfig& config);
grpc::Status ValidateGroupsConfig(const AGCProto::GroupsConfig& config);

}  // namespace AGC
