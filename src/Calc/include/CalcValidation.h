#pragma once

#include <grpcpp/support/status.h>

#include "Calc.pb.h"

namespace Calc {

grpc::Status ValidateGroupConfig(const CalcProto::CalcGroupConfig &config);
grpc::Status ValidateGroupsConfig(const CalcProto::GroupsConfig &config);

}  // namespace Calc
