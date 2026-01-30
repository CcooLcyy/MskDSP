#include "MQTTTopicMatcher.hpp"

#include <vector>

namespace MQTTManager {
namespace {
std::vector<std::string> splitTopic(const std::string& topic) {
  std::vector<std::string> parts;
  std::string current;
  for (char ch : topic) {
    if (ch == '/') {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  parts.push_back(current);
  return parts;
}
}  // namespace

bool MatchTopicFilter(const std::string& filter, const std::string& topic) {
  if (filter == "#") {
    return true;
  }
  const auto filterParts = splitTopic(filter);
  const auto topicParts = splitTopic(topic);
  size_t i = 0;
  size_t j = 0;
  for (; i < filterParts.size() && j < topicParts.size(); ++i, ++j) {
    const auto& fp = filterParts[i];
    if (fp == "#") {
      return true;
    }
    if (fp == "+") {
      continue;
    }
    if (fp != topicParts[j]) {
      return false;
    }
  }
  if (i == filterParts.size() && j == topicParts.size()) {
    return true;
  }
  if (i + 1 == filterParts.size() && filterParts[i] == "#") {
    return true;
  }
  return false;
}
}  // namespace MQTTManager
