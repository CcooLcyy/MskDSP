#pragma once

#include <boost/json.hpp>

#include <cctype>
#include <limits>
#include <string>
#include <vector>

namespace MQTTManager {
namespace JsonPath {
struct Segment {
  enum class Kind { kKey, kIndex };
  Kind kind;
  std::string key;
  size_t index{0};
};

inline bool parsePath(const std::string& path, std::vector<Segment>* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "解析路径失败";
    }
    return false;
  }
  out->clear();
  if (path.empty()) {
    if (error != nullptr) {
      *error = "匹配字段路径为空";
    }
    return false;
  }
  size_t i = 0;
  while (i < path.size()) {
    if (path[i] == '.') {
      if (error != nullptr) {
        *error = "匹配字段路径包含空段";
      }
      return false;
    }
    if (path[i] == '[') {
      ++i;
      if (i >= path.size()) {
        if (error != nullptr) {
          *error = "匹配字段路径数组下标缺失";
        }
        return false;
      }
      if (!std::isdigit(static_cast<unsigned char>(path[i]))) {
        if (error != nullptr) {
          *error = "匹配字段路径数组下标必须为数字";
        }
        return false;
      }
      size_t index = 0;
      while (i < path.size() && std::isdigit(static_cast<unsigned char>(path[i]))) {
        const size_t digit = static_cast<size_t>(path[i] - '0');
        if (index > (std::numeric_limits<size_t>::max() - digit) / 10) {
          if (error != nullptr) {
            *error = "匹配字段路径数组下标过大";
          }
          return false;
        }
        index = index * 10 + digit;
        ++i;
      }
      if (i >= path.size() || path[i] != ']') {
        if (error != nullptr) {
          *error = "匹配字段路径数组下标缺少右括号";
        }
        return false;
      }
      ++i;
      out->push_back(Segment{Segment::Kind::kIndex, "", index});
    } else {
      const size_t start = i;
      while (i < path.size() && path[i] != '.' && path[i] != '[') {
        ++i;
      }
      if (start == i) {
        if (error != nullptr) {
          *error = "匹配字段路径包含空段";
        }
        return false;
      }
      out->push_back(Segment{Segment::Kind::kKey, path.substr(start, i - start), 0});
    }

    if (i == path.size()) {
      break;
    }
    if (path[i] == '.') {
      ++i;
      if (i == path.size()) {
        if (error != nullptr) {
          *error = "匹配字段路径以点结尾";
        }
        return false;
      }
      continue;
    }
    if (path[i] == '[') {
      continue;
    }
    if (error != nullptr) {
      *error = "匹配字段路径包含非法字符";
    }
    return false;
  }
  return true;
}

inline bool extractValue(const boost::json::value& root, const std::vector<Segment>& path,
                         const boost::json::value** out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "提取字段失败";
    }
    return false;
  }
  const boost::json::value* current = &root;
  for (const auto& seg : path) {
    if (seg.kind == Segment::Kind::kKey) {
      if (!current->is_object()) {
        if (error != nullptr) {
          *error = "字段路径期望对象";
        }
        return false;
      }
      const auto& obj = current->as_object();
      const auto it = obj.find(seg.key);
      if (it == obj.end()) {
        if (error != nullptr) {
          *error = "字段路径不存在";
        }
        return false;
      }
      current = &it->value();
    } else {
      if (!current->is_array()) {
        if (error != nullptr) {
          *error = "字段路径期望数组";
        }
        return false;
      }
      const auto& arr = current->as_array();
      if (seg.index >= arr.size()) {
        if (error != nullptr) {
          *error = "字段路径数组下标越界";
        }
        return false;
      }
      current = &arr[seg.index];
    }
  }
  *out = current;
  return true;
}
}  // namespace JsonPath
}  // namespace MQTTManager
