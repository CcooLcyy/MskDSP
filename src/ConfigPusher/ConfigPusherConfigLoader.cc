#include "ConfigPusherConfigLoader.h"

#include <google/protobuf/util/json_util.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

#include "Logger.h"
#include "ModbusRTU.pb.h"

namespace ConfigPusher {
namespace {
std::string stripJsonComments(std::string_view input) {
  std::string out;
  out.reserve(input.size());

  bool inString = false;
  bool escape = false;
  bool inLineComment = false;
  bool inBlockComment = false;

  for (size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];

    if (inLineComment) {
      if (c == '\n') {
        inLineComment = false;
        out.push_back(c);
      }
      continue;
    }

    if (inBlockComment) {
      if (c == '*' && i + 1 < input.size() && input[i + 1] == '/') {
        inBlockComment = false;
        ++i;
        continue;
      }
      if (c == '\n') {
        out.push_back(c);
      }
      continue;
    }

    if (inString) {
      out.push_back(c);
      if (escape) {
        escape = false;
        continue;
      }
      if (c == '\\') {
        escape = true;
        continue;
      }
      if (c == '"') {
        inString = false;
      }
      continue;
    }

    if (c == '"') {
      inString = true;
      out.push_back(c);
      continue;
    }

    if (c == '/' && i + 1 < input.size()) {
      const char next = input[i + 1];
      if (next == '/') {
        inLineComment = true;
        ++i;
        continue;
      }
      if (next == '*') {
        inBlockComment = true;
        ++i;
        continue;
      }
    }

    out.push_back(c);
  }

  return out;
}

bool parseHexFunctionCode(std::string_view text, uint32_t *out) {
  if (out == nullptr) {
    return false;
  }
  if (text.size() < 3) {
    return false;
  }
  if (!(text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))) {
    return false;
  }
  uint32_t value = 0;
  for (size_t i = 2; i < text.size(); ++i) {
    const char c = text[i];
    uint32_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<uint32_t>(c - 'A' + 10);
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  *out = value;
  return true;
}

std::optional<int> mapModbusFunctionCode(uint32_t code) {
  if (code == 0x01) {
    return static_cast<int>(ModbusRTUProto::FUNCTION_READ_COILS);
  }
  if (code == 0x03) {
    return static_cast<int>(ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS);
  }
  return std::nullopt;
}

std::string normalizeModbusFunctionCodes(std::string_view input, size_t *outConverted) {
  if (outConverted != nullptr) {
    *outConverted = 0;
  }
  static const std::regex kHexFunctionRegex(R"rx("function"\s*:\s*"((0[xX])[0-9a-fA-F]+)")rx");
  std::string out;
  out.reserve(input.size());

  auto begin = input.begin();
  auto end = input.end();
  std::match_results<std::string_view::const_iterator> match;
  while (std::regex_search(begin, end, match, kHexFunctionRegex)) {
    out.append(begin, match.prefix().second);
    std::string hexText(match[1].first, match[1].second);
    uint32_t code = 0;
    if (!parseHexFunctionCode(hexText, &code)) {
      out.append(match[0].first, match[0].second);
      begin = match.suffix().first;
      continue;
    }
    const auto mapped = mapModbusFunctionCode(code);
    if (mapped.has_value()) {
      out.append("\"function\": ");
      out.append(std::to_string(mapped.value()));
      if (outConverted != nullptr) {
        *outConverted += 1;
      }
    } else {
      LOG_WARNING("ConfigPusher 发现不支持的 ModbusRTU 功能码十六进制写法: {}", hexText);
      out.append(match[0].first, match[0].second);
    }
    begin = match.suffix().first;
  }
  out.append(begin, end);
  return out;
}

bool readFile(const std::filesystem::path &path, std::string *out) {
  if (out == nullptr) {
    return false;
  }
  std::ifstream ifs(path, std::ios::in | std::ios::binary);
  if (!ifs.is_open()) {
    return false;
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  *out = oss.str();
  return true;
}
}  // namespace

std::optional<ConfigPusherProto::Config> LoadConfigFile(const std::filesystem::path &path) {
  if (!std::filesystem::exists(path)) {
    LOG_INFO("未找到 ConfigPusher 配置文件: {}", path.string());
    return std::nullopt;
  }

  LOG_INFO("开始读取 ConfigPusher 配置文件: {}", path.string());
  std::string raw;
  if (!readFile(path, &raw)) {
    LOG_ERROR("读取 ConfigPusher 配置文件失败: {}", path.string());
    return std::nullopt;
  }

  auto json = stripJsonComments(raw);
  if (path.filename() == "modbus_rtu.jsonc") {
    size_t converted = 0;
    json = normalizeModbusFunctionCodes(json, &converted);
    if (converted > 0) {
      LOG_INFO("ConfigPusher 已将 ModbusRTU 功能码十六进制写法转换为枚举值: 数量={}", converted);
    }
  }
  ConfigPusherProto::Config config;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = false;
  auto parseStatus = google::protobuf::util::JsonStringToMessage(json, &config, options);
  if (!parseStatus.ok()) {
    LOG_ERROR("解析 ConfigPusher 配置失败: {}", parseStatus.ToString());
    return std::nullopt;
  }
  LOG_INFO("ConfigPusher 配置解析成功: {}", path.string());
  return config;
}

std::optional<ConfigPusherProto::DataCenterConfig> LoadDataCenterConfigFile(const std::filesystem::path &path) {
  if (!std::filesystem::exists(path)) {
    LOG_INFO("未找到 DataCenter 配置文件: {}", path.string());
    return std::nullopt;
  }

  LOG_INFO("开始读取 DataCenter 配置文件: {}", path.string());
  std::string raw;
  if (!readFile(path, &raw)) {
    LOG_ERROR("读取 DataCenter 配置文件失败: {}", path.string());
    return std::nullopt;
  }

  auto json = stripJsonComments(raw);
  ConfigPusherProto::DataCenterConfig config;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = false;
  auto parseStatus = google::protobuf::util::JsonStringToMessage(json, &config, options);
  if (!parseStatus.ok()) {
    LOG_ERROR("解析 DataCenter 配置失败: {}", parseStatus.ToString());
    return std::nullopt;
  }
  LOG_INFO("DataCenter 配置解析成功: {}", path.string());
  return config;
}
}  // namespace ConfigPusher
