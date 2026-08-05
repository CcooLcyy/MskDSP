#pragma once

#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850.pb.h"

namespace IEC61850 {

grpc::Status ValidatePersistedConfig(
    const IEC61850Proto::PersistedConfig& config,
    std::vector<IEC61850Proto::ValidationIssue>* issues = nullptr);

}  // namespace IEC61850
