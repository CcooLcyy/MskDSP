#pragma once

#include <cstddef>
#include <filesystem>

#include "ConfigPusher.pb.h"
#include "IEC61850.grpc.pb.h"

namespace ConfigPusher {

bool applyIec61850Config(
    const ConfigPusherProto::Iec61850Config& config,
    const std::filesystem::path& configFilePath,
    IEC61850Proto::IEC61850Service::StubInterface* stub);

bool applyIec61850Config(
    const ConfigPusherProto::Iec61850Config& config,
    const std::filesystem::path& configFilePath,
    IEC61850Proto::IEC61850Service::StubInterface* stub,
    std::size_t maxSerializedRequestBytes);

}  // namespace ConfigPusher
