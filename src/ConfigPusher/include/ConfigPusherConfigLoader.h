#pragma once

#include <filesystem>
#include <optional>

#include "ConfigPusher.pb.h"

namespace ConfigPusher {
std::optional<ConfigPusherProto::Config> LoadConfigFile(const std::filesystem::path &path);
std::optional<ConfigPusherProto::DataCenterConfig> LoadDataCenterConfigFile(const std::filesystem::path &path);
}  // namespace ConfigPusher
