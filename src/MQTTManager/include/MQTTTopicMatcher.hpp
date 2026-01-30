#pragma once

#include <string>

namespace MQTTManager {
bool MatchTopicFilter(const std::string& filter, const std::string& topic);
}  // namespace MQTTManager
