#pragma once

#include <cstddef>
#include <string_view>

#include <grpcpp/support/status.h>

#include "IEC61850.pb.h"

namespace IEC61850 {

std::size_t CountServerAccessPoints(const IEC61850Proto::SclIed& ied);
bool HasUnscopedAccessPointObjects(const IEC61850Proto::SclIed& ied);
bool BelongsToAccessPoint(std::string_view objectAccessPoint,
                          std::string_view selectedAccessPoint,
                          std::size_t serverAccessPointCount);
std::size_t NormalizeSingleServerAccessPointOwnership(
    IEC61850Proto::SclIed* ied);
grpc::Status BuildAccessPointIedModel(
    const IEC61850Proto::SclIed& source,
    std::string_view accessPoint,
    IEC61850Proto::SclIed* selected);

}  // namespace IEC61850
