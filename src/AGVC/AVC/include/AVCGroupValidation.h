#pragma once

#include <grpcpp/support/status.h>

#include "AVC.pb.h"

namespace AVC {

grpc::Status ValidateGroupConfig(const AVCProto::GroupConfig& config);
grpc::Status ValidateGroupsConfig(const AVCProto::GroupsConfig& config);

}  // namespace AVC
