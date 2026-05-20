#include "ModuleManager.h"

#include <dlfcn.h>
#include <google/protobuf/stubs/common.h>
#include <grpcpp/completion_queue.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/service_type.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <link.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/dll/shared_library.hpp>
#include <boost/json.hpp>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Logger.h"
#include "ModuleInterface.h"
#include "ModuleManager.pb.h"
#include "ModuleManagerGrpcService.h"
#include "moduleManagerLibInfo.h"

namespace {
constexpr const char *kAutoStartConfigPath = "./conf/module_manager.jsonc";
constexpr const char *kBootConfigModeEnvName = "MSKDSP_BOOT_CONFIG_MODE";
constexpr const char *kBootConfigModeConfigPusher = "CONFIG_PUSHER";
constexpr const char *kBootConfigModeUpper = "UPPER";
constexpr const char *kConfigPusherModuleName = "ConfigPusher";

struct ModuleTraceAutoStartRule {
  std::string_view moduleName;
  std::vector<std::filesystem::path> tracePaths;
};

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

struct VersionConstraint {
  enum class Op {
    kEq,
    kLt,
    kLte,
    kGt,
    kGte,
  };
  Op op;
  std::vector<int> version;
};

std::string joinNames(const std::vector<std::string> &names, std::string_view sep) {
  if (names.empty()) {
    return {};
  }
  std::ostringstream oss;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i != 0) {
      oss << sep;
    }
    oss << names[i];
  }
  return oss.str();
}

std::string toHex(std::string_view text) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (unsigned char c : text) {
    oss << std::setw(2) << static_cast<int>(c);
  }
  return oss.str();
}

std::string joinPaths(const std::vector<std::filesystem::path> &paths, std::string_view sep) {
  std::vector<std::string> texts;
  texts.reserve(paths.size());
  for (const auto &path : paths) {
    texts.push_back(path.string());
  }
  return joinNames(texts, sep);
}

std::filesystem::path makeBackupPath(const std::filesystem::path &path) {
  return std::filesystem::path(path.string() + ".bak");
}

std::filesystem::path makeTmpPath(const std::filesystem::path &path) {
  return std::filesystem::path(path.string() + ".tmp");
}

std::vector<ModuleTraceAutoStartRule> buildUpperModeTraceAutoStartRules() {
  return {
      {"DataCenter",
       {"./conf/dataCenter/connections.pb", "./conf/dataCenter/conn_tags.pb", "./conf/dataCenter/routes.pb", "./conf/dataCenter/point_tables.pb"}},
      {"IEC104", {"./conf/IEC104/links.pb", "./conf/IEC104/point_tables.pb"}},
      {"ModbusRTU", {"./conf/ModbusRTU/mqtt.pb", "./conf/ModbusRTU/links.pb", "./conf/ModbusRTU/point_tables.pb"}},
      {"DLT645", {"./conf/DLT645/mqtt.pb", "./conf/DLT645/links.pb", "./conf/DLT645/point_tables.pb"}},
      {"AGC", {"./conf/AGC/groups.pb"}},
      {"AVC", {"./conf/AVC/groups.pb"}}};
}

std::vector<std::filesystem::path> collectMatchedPersistentTracePaths(const std::vector<std::filesystem::path> &tracePaths) {
  std::vector<std::filesystem::path> matched;
  matched.reserve(tracePaths.size() * 2);
  for (const auto &path : tracePaths) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      matched.push_back(path);
    }
    const auto backupPath = makeBackupPath(path);
    ec.clear();
    if (std::filesystem::exists(backupPath, ec)) {
      matched.push_back(backupPath);
    }
  }
  return matched;
}

std::vector<std::pair<std::string, std::vector<std::filesystem::path>>> collectUpperModeAutoStartModulesByTrace() {
  std::vector<std::pair<std::string, std::vector<std::filesystem::path>>> modules;
  for (const auto &rule : buildUpperModeTraceAutoStartRules()) {
    auto matched = collectMatchedPersistentTracePaths(rule.tracePaths);
    if (matched.empty()) {
      continue;
    }
    modules.emplace_back(std::string(rule.moduleName), std::move(matched));
  }
  return modules;
}

void clearManagedPersistentTraceFilesForConfigPusher() {
  LOG_INFO("当前 boot_config_mode={}，开始在模块启动前清理受管持久化文件", kBootConfigModeConfigPusher);
  size_t removed = 0;
  size_t missing = 0;
  size_t failed = 0;

  for (const auto &rule : buildUpperModeTraceAutoStartRules()) {
    for (const auto &path : rule.tracePaths) {
      const std::array<std::filesystem::path, 3> cleanupTargets = {
          path,
          makeBackupPath(path),
          makeTmpPath(path),
      };
      for (const auto &target : cleanupTargets) {
        std::error_code ec;
        const bool exists = std::filesystem::exists(target, ec);
        if (ec) {
          ++failed;
          LOG_ERROR("CONFIG_PUSHER 启动前检查持久化文件失败: 模块={}, 文件={}, 原因={}",
                    rule.moduleName,
                    target.string(),
                    ec.message());
          continue;
        }
        if (!exists) {
          ++missing;
          continue;
        }
        const bool deleted = std::filesystem::remove(target, ec);
        if (ec || !deleted) {
          ++failed;
          LOG_ERROR("CONFIG_PUSHER 启动前删除持久化文件失败: 模块={}, 文件={}, 原因={}",
                    rule.moduleName,
                    target.string(),
                    ec ? ec.message() : "删除结果为 false");
          continue;
        }
        ++removed;
        LOG_WARNING("CONFIG_PUSHER 启动前已删除受管持久化文件: 模块={}, 文件={}",
                    rule.moduleName,
                    target.string());
      }
    }
  }

  LOG_INFO("当前 boot_config_mode={}，模块启动前受管持久化文件清理完成: 删除={}, 缺失={}, 失败={}",
           kBootConfigModeConfigPusher,
           removed,
           missing,
           failed);
}

void logJsonFieldDetails(std::string_view title, const boost::json::object &obj, std::string_view phase, std::string_view moduleName) {
  LOG_INFO("{}字段数量={}，阶段: {}，模块: {}", title, obj.size(), phase, moduleName);
  if (obj.empty()) {
    return;
  }
  for (const auto &entry : obj) {
    const auto &key = entry.key();
    std::string keyText(key.data(), key.size());
    LOG_INFO("{}字段明细: 名称='{}'，字节数={}，十六进制={}，阶段: {}，模块: {}", title, keyText, keyText.size(), toHex(keyText), phase, moduleName);
  }
}

std::string sanitizeJsonSnippet(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
    case '\n':
      out.append("\\n");
      break;
    case '\r':
      out.append("\\r");
      break;
    case '\t':
      out.append("\\t");
      break;
    default:
      out.push_back(ch);
      break;
    }
  }
  return out;
}

void logJsonParseError(std::string_view title, std::string_view phase, std::string_view moduleName, std::string_view json, std::size_t offset, int errorValue, bool warnOnly) {
  const std::size_t safeOffset = std::min(offset, json.size());
  std::size_t line = 1;
  std::size_t column = 1;
  for (std::size_t i = 0; i < safeOffset; ++i) {
    const char ch = json[i];
    if (ch == '\n') {
      ++line;
      column = 1;
    } else if (ch == '\r') {
      ++line;
      column = 1;
      if (i + 1 < safeOffset && json[i + 1] == '\n') {
        ++i;
      }
    } else {
      ++column;
    }
  }
  constexpr std::size_t kSnippetRadius = 20;
  const std::size_t snippetStart = safeOffset > kSnippetRadius ? safeOffset - kSnippetRadius : 0;
  const std::size_t snippetEnd = std::min(json.size(), safeOffset + kSnippetRadius);
  const auto snippet = sanitizeJsonSnippet(json.substr(snippetStart, snippetEnd - snippetStart));
  if (warnOnly) {
    LOG_WARNING("{}解析失败: JSON 语法错误码={}，行={}，列={}，偏移={}，附近文本='{}'，阶段: {}，模块: {}", title, errorValue, line, column, safeOffset, snippet, phase, moduleName);
  } else {
    LOG_ERROR("{}解析失败: JSON 语法错误码={}，行={}，列={}，偏移={}，附近文本='{}'，阶段: {}，模块: {}", title, errorValue, line, column, safeOffset, snippet, phase, moduleName);
  }
}

bool parseJsonValue(std::string_view json, boost::json::value *out, std::string_view title, std::string_view phase, std::string_view moduleName, bool warnOnly) {
  if (out == nullptr) {
    logJsonParseError(title, phase, moduleName, json, 0, -1, warnOnly);
    return false;
  }
  boost::json::parser parser;
  boost::system::error_code ec;
  const std::size_t consumed = parser.write(json, ec);
  if (ec) {
    logJsonParseError(title, phase, moduleName, json, consumed, ec.value(), warnOnly);
    return false;
  }
  *out = parser.release();
  return true;
}

uint64_t hashBytes(const uint8_t *data, size_t size) {
  constexpr uint64_t kOffset = 14695981039346656037ull;
  constexpr uint64_t kPrime = 1099511628211ull;
  uint64_t hash = kOffset;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= kPrime;
  }
  return hash;
}

class ModuleInfosBuildGuard {
public:
  ModuleInfosBuildGuard(std::atomic_bool &flag, std::string_view reason) :
    flag_(flag) {
    bool expected = false;
    acquired_ = flag_.compare_exchange_strong(expected, true);
    if (!acquired_) {
      LOG_WARNING("模块清单正在构建，检测到并发访问: {}", reason);
    }
  }
  ~ModuleInfosBuildGuard() {
    if (acquired_) {
      flag_.store(false);
    }
  }
  bool acquired() const {
    return acquired_;
  }

private:
  std::atomic_bool &flag_;
  bool acquired_{false};
};

bool isRuntimeLibName(std::string_view path) {
  return path.find("libprotobuf") != std::string_view::npos ||
      path.find("libgrpc++") != std::string_view::npos ||
      path.find("libgrpc") != std::string_view::npos ||
      path.find("libgpr") != std::string_view::npos ||
      path.find("libabsl") != std::string_view::npos ||
      path.find("libupb") != std::string_view::npos ||
      path.find("libstdc++") != std::string_view::npos ||
      path.find("libgcc_s") != std::string_view::npos ||
      path.find("libatomic") != std::string_view::npos;
}

void logLoadedRuntimeLibs(std::string_view phase, std::string_view moduleName, bool force) {
  struct LoadedLibs {
    std::unordered_set<std::string> paths;
  } data;

  dl_iterate_phdr(
      [](struct dl_phdr_info *info, size_t, void *userData) -> int {
        if (info == nullptr || info->dlpi_name == nullptr) {
          return 0;
        }
        std::string_view path(info->dlpi_name);
        if (path.empty()) {
          return 0;
        }
        if (!isRuntimeLibName(path)) {
          return 0;
        }
        auto *loaded = static_cast<LoadedLibs *>(userData);
        loaded->paths.emplace(path);
        return 0;
      },
      &data);

  static std::unordered_set<std::string> lastPaths;
  std::vector<std::string> current(data.paths.begin(), data.paths.end());
  std::sort(current.begin(), current.end());
  if (force) {
    LOG_INFO("已加载运行库列表，阶段: {}，模块: {}，数量: {}，明细: {}", phase, moduleName, current.size(), joinNames(current, ", "));
    lastPaths = std::move(data.paths);
    return;
  }
  std::vector<std::string> added;
  for (const auto &path : current) {
    if (lastPaths.find(path) == lastPaths.end()) {
      added.push_back(path);
    }
  }
  if (!added.empty()) {
    LOG_WARNING("检测到新增运行库，阶段: {}，模块: {}，明细: {}", phase, moduleName, joinNames(added, ", "));
    lastPaths.insert(added.begin(), added.end());
  }
}

std::string extractSoVersion(std::string_view path, std::string_view soName) {
  const auto pos = path.rfind(soName);
  if (pos == std::string_view::npos) {
    return {};
  }
  const auto versionPos = pos + soName.size();
  if (versionPos >= path.size()) {
    return {};
  }
  return std::string(path.substr(versionPos));
}

std::string resolveProtobufRuntimePath() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void *>(&google::protobuf::internal::VersionString), &info) != 0 &&
      info.dli_fname != nullptr) {
    return info.dli_fname;
  }
  return {};
}

void logProtobufVersionInfo() {
  const auto compiledVersion = google::protobuf::internal::VersionString(GOOGLE_PROTOBUF_VERSION);
  LOG_INFO("protobuf 编译期版本: {}{}", compiledVersion, GOOGLE_PROTOBUF_VERSION_SUFFIX);
  LOG_INFO("protobuf 编译期版本号: {}", GOOGLE_PROTOBUF_VERSION);
  const auto runtimePath = resolveProtobufRuntimePath();
  if (runtimePath.empty()) {
    LOG_WARNING("protobuf 运行时库路径解析失败");
    return;
  }
  LOG_INFO("protobuf 运行时库路径: {}", runtimePath);
  const auto runtimeVersion = extractSoVersion(runtimePath, "libprotobuf.so.");
  if (runtimeVersion.empty()) {
    LOG_WARNING("protobuf 运行时库版本解析失败");
  } else {
    LOG_INFO("protobuf 运行时库版本: {}", runtimeVersion);
  }
}

bool selfCheckAutoStartConfig(std::string_view phase, std::string_view moduleName, bool logOnSuccess) {
  static const std::string kSelfCheckJson = R"({"auto_start_modules":["ConfigPusher"]})";
  boost::json::value parsed;
  if (!parseJsonValue(kSelfCheckJson, &parsed, "自动启动配置自检", phase, moduleName, true)) {
    return false;
  }
  if (!parsed.is_object()) {
    LOG_WARNING("自动启动配置自检结果不是对象，阶段: {}，模块: {}", phase, moduleName);
    return false;
  }
  const auto &obj = parsed.as_object();
  auto fieldIt = obj.find("auto_start_modules");
  if (fieldIt == obj.end()) {
    LOG_WARNING("自动启动配置自检未找到字段 auto_start_modules，阶段: {}，模块: {}", phase, moduleName);
    logJsonFieldDetails("自动启动配置自检", obj, phase, moduleName);
    return false;
  }
  if (!fieldIt->value().is_array()) {
    LOG_WARNING("自动启动配置自检字段类型异常，阶段: {}，模块: {}", phase, moduleName);
    logJsonFieldDetails("自动启动配置自检", obj, phase, moduleName);
    return false;
  }
  if (logOnSuccess) {
    LOG_INFO("自动启动配置自检通过，阶段: {}，模块: {}", phase, moduleName);
  }
  return true;
}

std::string parseBootConfigMode(const boost::json::object &obj) {
  auto fieldIt = obj.find("boot_config_mode");
  if (fieldIt == obj.end()) {
    LOG_INFO("自动启动配置未包含 boot_config_mode，使用默认值: {}", kBootConfigModeConfigPusher);
    return kBootConfigModeConfigPusher;
  }
  if (!fieldIt->value().is_string()) {
    LOG_ERROR("自动启动配置的 boot_config_mode 必须为字符串，已回退为安全模式: {}", kBootConfigModeUpper);
    return kBootConfigModeUpper;
  }

  const auto &modeValue = fieldIt->value().as_string();
  std::string mode(modeValue.data(), modeValue.size());
  if (mode == kBootConfigModeConfigPusher || mode == kBootConfigModeUpper) {
    LOG_INFO("自动启动配置解析到 boot_config_mode: {}", mode);
    return mode;
  }

  LOG_ERROR("自动启动配置的 boot_config_mode 非法: {}，已回退为安全模式: {}", mode, kBootConfigModeUpper);
  return kBootConfigModeUpper;
}

void setProcessBootConfigMode(std::string_view mode) {
  std::string modeText(mode);
  if (::setenv(kBootConfigModeEnvName, modeText.c_str(), 1) != 0) {
    LOG_ERROR("写入 boot_config_mode 环境变量失败: 变量={}, 模式={}", kBootConfigModeEnvName, modeText);
    return;
  }
  LOG_INFO("本次进程已固定 boot_config_mode: {}（环境变量: {}）", modeText, kBootConfigModeEnvName);
}

std::string extractModuleNameFromLibName(const std::string &libName) {
  auto soPos = libName.find(".so");
  if (soPos == std::string::npos || soPos <= 3) {
    return {};
  }
  if (libName.rfind("lib", 0) != 0) {
    return {};
  }
  return libName.substr(3, soPos - 3);
}

bool parseVersionParts(const std::string &version, std::vector<int> *parts, std::string *error) {
  if (parts == nullptr) {
    return false;
  }
  parts->clear();
  if (version.empty()) {
    if (error != nullptr) {
      *error = "版本为空";
    }
    return false;
  }

  size_t start = 0;
  while (start < version.size()) {
    size_t end = version.find('.', start);
    auto segment = version.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (segment.empty()) {
      if (error != nullptr) {
        *error = "版本段为空";
      }
      return false;
    }
    for (const auto ch : segment) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        if (error != nullptr) {
          *error = "版本段不是数字";
        }
        return false;
      }
    }
    parts->push_back(std::stoi(segment));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

int compareVersions(const std::vector<int> &lhs, const std::vector<int> &rhs) {
  const auto maxSize = std::max(lhs.size(), rhs.size());
  for (size_t i = 0; i < maxSize; ++i) {
    const auto left = i < lhs.size() ? lhs[i] : 0;
    const auto right = i < rhs.size() ? rhs[i] : 0;
    if (left < right) {
      return -1;
    }
    if (left > right) {
      return 1;
    }
  }
  return 0;
}

bool parseVersionRange(const std::string &expr, std::vector<VersionConstraint> *constraints, std::string *error) {
  if (constraints == nullptr) {
    return false;
  }
  constraints->clear();
  if (expr.empty()) {
    return true;
  }

  std::istringstream iss(expr);
  std::string token;
  while (iss >> token) {
    std::string op;
    std::string version;
    if (token == "<" || token == "<=" || token == ">" || token == ">=" || token == "=") {
      op = token;
      if (!(iss >> version)) {
        if (error != nullptr) {
          *error = "操作符后缺少版本号";
        }
        return false;
      }
    } else {
      if (token.rfind("==", 0) == 0) {
        if (error != nullptr) {
          *error = "不支持操作符 '=='";
        }
        return false;
      }
      if (token.rfind(">=", 0) == 0 || token.rfind("<=", 0) == 0) {
        op = token.substr(0, 2);
        version = token.substr(2);
      } else if (!token.empty() && (token[0] == '>' || token[0] == '<' || token[0] == '=')) {
        op = token.substr(0, 1);
        version = token.substr(1);
      } else {
        op = "=";
        version = token;
      }
    }
    if (version.empty()) {
      if (error != nullptr) {
        *error = "约束中缺少版本号";
      }
      return false;
    }

    std::vector<int> parts;
    std::string parseError;
    if (!parseVersionParts(version, &parts, &parseError)) {
      if (error != nullptr) {
        *error = parseError;
      }
      return false;
    }

    VersionConstraint::Op opValue = VersionConstraint::Op::kEq;
    if (op == "=") {
      opValue = VersionConstraint::Op::kEq;
    } else if (op == "<") {
      opValue = VersionConstraint::Op::kLt;
    } else if (op == "<=") {
      opValue = VersionConstraint::Op::kLte;
    } else if (op == ">") {
      opValue = VersionConstraint::Op::kGt;
    } else if (op == ">=") {
      opValue = VersionConstraint::Op::kGte;
    } else {
      if (error != nullptr) {
        *error = "未知操作符";
      }
      return false;
    }
    constraints->push_back({opValue, std::move(parts)});
  }

  if (constraints->empty()) {
    if (error != nullptr) {
      *error = "版本约束为空";
    }
    return false;
  }
  return true;
}

bool isVersionSatisfied(const std::vector<int> &version, const std::vector<VersionConstraint> &constraints) {
  for (const auto &constraint : constraints) {
    const auto cmp = compareVersions(version, constraint.version);
    switch (constraint.op) {
    case VersionConstraint::Op::kEq:
      if (cmp != 0) {
        return false;
      }
      break;
    case VersionConstraint::Op::kLt:
      if (cmp >= 0) {
        return false;
      }
      break;
    case VersionConstraint::Op::kLte:
      if (cmp > 0) {
        return false;
      }
      break;
    case VersionConstraint::Op::kGt:
      if (cmp <= 0) {
        return false;
      }
      break;
    case VersionConstraint::Op::kGte:
      if (cmp < 0) {
        return false;
      }
      break;
    }
  }
  return true;
}

bool loadModuleManifest(const std::filesystem::path &libPath, ModuleManagerProto::ModuleManifest *manifest, std::string *error, std::unordered_map<std::string, boost::dll::shared_library> *libCache) {
  if (manifest == nullptr) {
    return false;
  }
  LOG_INFO("读取模块 manifest 开始: {}", libPath.string());
  selfCheckAutoStartConfig("加载模块库前", libPath.filename().string(), false);
  try {
    const auto cacheKey = libPath.string();
    boost::dll::shared_library localLib;
    boost::dll::shared_library *lib = nullptr;
    if (libCache != nullptr) {
      auto cachedIt = libCache->find(cacheKey);
      if (cachedIt != libCache->end() && cachedIt->second.is_loaded()) {
        lib = &cachedIt->second;
        LOG_INFO("模块库句柄已缓存，复用: {}", libPath.filename().string());
      }
    }
    if (lib == nullptr) {
      LOG_INFO("加载模块库: {}", libPath.string());
      if (libCache != nullptr) {
        auto [it, inserted] = libCache->try_emplace(cacheKey);
        if (!it->second.is_loaded()) {
          it->second.load(libPath.string(), boost::dll::load_mode::rtld_lazy);
          LOG_INFO("已缓存模块库句柄，避免卸载引发异常: {}", libPath.filename().string());
        } else if (!inserted) {
          LOG_INFO("模块库句柄已缓存，跳过重复缓存: {}", libPath.filename().string());
        }
        lib = &it->second;
      } else {
        localLib.load(libPath.string(), boost::dll::load_mode::rtld_lazy);
        lib = &localLib;
      }
    }
    LOG_INFO("模块库加载完成: {}", libPath.filename().string());
    logLoadedRuntimeLibs("加载模块库后", libPath.filename().string(), false);
    selfCheckAutoStartConfig("加载模块库后", libPath.filename().string(), false);
    LOG_INFO("检查 manifest 符号: {}", libPath.filename().string());
    if (!lib->has("GetModuleManifestPb")) {
      if (error != nullptr) {
        *error = "未找到 manifest 符号 GetModuleManifestPb";
      }
      return false;
    }
    using ManifestFn = bool(const uint8_t **, size_t *);
    auto &fn = lib->get<ManifestFn>("GetModuleManifestPb");
    LOG_INFO("manifest 符号解析成功: {}", libPath.filename().string());
    LOG_INFO("调用 manifest 函数: {}", libPath.filename().string());
    selfCheckAutoStartConfig("调用 manifest 前", libPath.filename().string(), false);
    const uint8_t *data = nullptr;
    size_t size = 0;
    if (!fn(&data, &size)) {
      if (error != nullptr) {
        *error = "manifest 函数返回 false";
      }
      return false;
    }
    if (data == nullptr || size == 0) {
      if (error != nullptr) {
        *error = "manifest 返回空数据";
      }
      return false;
    }
    selfCheckAutoStartConfig("获取 manifest 后", libPath.filename().string(), false);
    logLoadedRuntimeLibs("获取 manifest 后", libPath.filename().string(), false);
    const uint64_t hashOnce = hashBytes(data, size);
    const uint8_t *dataAgain = nullptr;
    size_t sizeAgain = 0;
    const bool againOk = fn(&dataAgain, &sizeAgain);
    if (!againOk || dataAgain == nullptr || sizeAgain == 0) {
      LOG_WARNING("模块 {} manifest 二次获取失败，可能存在不稳定返回值", libPath.filename().string());
    } else if (dataAgain != data || sizeAgain != size) {
      LOG_WARNING("模块 {} manifest 二次获取结果不一致: ptr {} -> {}, size {} -> {}", libPath.filename().string(), static_cast<const void *>(data), static_cast<const void *>(dataAgain), size, sizeAgain);
    } else {
      const uint64_t hashAgain = hashBytes(dataAgain, sizeAgain);
      if (hashAgain != hashOnce) {
        LOG_WARNING("模块 {} manifest 内容在二次获取时发生变化，可能存在内存破坏或返回缓冲区复用", libPath.filename().string());
      }
    }
    LOG_INFO("解析 manifest 数据: {} bytes ({})", size, libPath.filename().string());
    if (!manifest->ParseFromArray(data, static_cast<int>(size))) {
      if (error != nullptr) {
        *error = "manifest 解析失败";
      }
      return false;
    }
    LOG_INFO("读取模块 manifest 成功: {}", libPath.filename().string());
    return true;
  } catch (const std::exception &ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
    LOG_ERROR("读取模块 manifest 异常: {} ({})", libPath.string(), ex.what());
    return false;
  }
}

bool validateManifest(const ModuleManagerProto::ModuleManifest &manifest, const std::string &expectedName, std::string *error) {
  if (manifest.module_name().empty()) {
    if (error != nullptr) {
      *error = "manifest 的 module_name 为空";
    }
    return false;
  }
  if (manifest.module_name() != expectedName) {
    if (error != nullptr) {
      *error = "manifest 的 module_name 不匹配: 期望 " + expectedName + " 实际 " + manifest.module_name();
    }
    return false;
  }
  if (manifest.version().version().empty()) {
    if (error != nullptr) {
      *error = "manifest 版本为空";
    }
    return false;
  }
  std::string parseError;
  std::vector<int> versionParts;
  if (!parseVersionParts(manifest.version().version(), &versionParts, &parseError)) {
    if (error != nullptr) {
      *error = "manifest 版本非法: " + parseError;
    }
    return false;
  }
  for (const auto &dependency : manifest.dependencies()) {
    if (dependency.module_name().empty()) {
      if (error != nullptr) {
        *error = "依赖模块 module_name 为空";
      }
      return false;
    }
    if (!dependency.version_range().empty()) {
      std::vector<VersionConstraint> constraints;
      if (!parseVersionRange(dependency.version_range(), &constraints, &parseError)) {
        if (error != nullptr) {
          *error = "依赖模块 " + dependency.module_name() + " 的 version_range 非法: " + parseError;
        }
        return false;
      }
    }
  }
  return true;
}
}  // namespace

namespace ModuleManager {
ModuleManager::ModuleManager() :
  moduleManagerService_(std::make_shared<ModuleManagerServiceImpl>()),
  ModuleInterface::ModuleInterface() {
  initLibInfo(moduleManagerLibInfo);
  releasePort(metaData_.outerGRPCServer);
  metaData_.outerGRPCServer = std::string("0.0.0.0:17000");
  reservePort(metaData_.outerGRPCServer);
}
ModuleManager::~ModuleManager() {}
void ModuleManager::start(std::stop_token stopToken) {
  LOG_INFO("正在启动模块管理器");
  const auto startTime = std::chrono::steady_clock::now();
  auto logElapsed = [&](std::string_view step) {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
    LOG_INFO("启动流程耗时: {} = {} ms", step, elapsed);
  };
  logElapsed("启动模块管理器入口");
  logProtobufVersionInfo();
  logElapsed("protobuf 版本信息输出完成");
  auto selfCheckBegin = std::chrono::steady_clock::now();
  LOG_INFO("开始自动启动配置自检，阶段: 启动 gRPC 前");
  selfCheckAutoStartConfig("启动 gRPC 前", metaData_.name, true);
  LOG_INFO("完成自动启动配置自检，阶段: 启动 gRPC 前，耗时: {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - selfCheckBegin).count());
  logElapsed("启动 gRPC 前自检完成");
  LOG_INFO("已跳过 gRPC 客户端符号预加载");
  logElapsed("gRPC 客户端符号预加载阶段结束");
  moduleManagerService_->getModuleManager(this);
  logElapsed("模块管理器注入到服务完成");
  auto grpcBuildBegin = std::chrono::steady_clock::now();
  LOG_INFO("开始构建 gRPC 服务");
  grpcServerBuilder(moduleManagerService_);
  LOG_INFO("完成构建 gRPC 服务，耗时: {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - grpcBuildBegin).count());
  logElapsed("gRPC 服务构建完成");
  logLoadedRuntimeLibs("gRPC 服务启动后", metaData_.name, true);
  logElapsed("gRPC 服务启动后运行库记录完成");
  selfCheckBegin = std::chrono::steady_clock::now();
  LOG_INFO("开始自动启动配置自检，阶段: 启动 gRPC 后");
  selfCheckAutoStartConfig("启动 gRPC 后", metaData_.name, true);
  LOG_INFO("完成自动启动配置自检，阶段: 启动 gRPC 后，耗时: {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - selfCheckBegin).count());
  logElapsed("启动 gRPC 后自检完成");
  auto moduleInfosBegin = std::chrono::steady_clock::now();
  LOG_INFO("开始扫描模块清单");
  initModuleInfos();
  LOG_INFO("完成扫描模块清单，耗时: {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - moduleInfosBegin).count());
  logElapsed("模块清单扫描完成");
  auto autoStartBegin = std::chrono::steady_clock::now();
  LOG_INFO("开始解析自动启动配置并启动模块");
  autoStartModulesFromConfig();
  LOG_INFO("完成解析自动启动配置并启动模块，耗时: {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - autoStartBegin).count());
  logElapsed("自动启动流程完成");
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
void ModuleManager::ensureModuleInfos() {
  if (moduleInfosBuilding_.load()) {
    LOG_WARNING("模块清单正在构建，确保模块清单时检测到并发访问");
  }
  if (!moduleInfosReady_) {
    initModuleInfos();
  }
}
void ModuleManager::autoStartModulesFromConfig() {
  const std::filesystem::path configPath(kAutoStartConfigPath);
  std::string bootConfigMode = kBootConfigModeConfigPusher;
  std::error_code cwdError;
  auto currentPath = std::filesystem::current_path(cwdError);
  if (cwdError) {
    LOG_WARNING("获取当前工作目录失败: {}", cwdError.message());
  } else {
    LOG_INFO("当前工作目录: {}", currentPath.string());
  }
  std::error_code absError;
  auto absConfigPath = std::filesystem::absolute(configPath, absError);
  if (absError) {
    LOG_WARNING("解析自动启动配置绝对路径失败: {}", absError.message());
  } else {
    LOG_INFO("自动启动配置绝对路径: {}", absConfigPath.string());
  }
  LOG_INFO("自动启动配置路径: {}", configPath.string());
  if (!std::filesystem::exists(configPath)) {
    setProcessBootConfigMode(bootConfigMode);
    LOG_INFO("未找到自动启动配置文件: {}", configPath.string());
    return;
  }
  std::error_code sizeError;
  auto fileSize = std::filesystem::file_size(configPath, sizeError);
  if (sizeError) {
    LOG_WARNING("获取自动启动配置文件大小失败: {}", sizeError.message());
  } else {
    LOG_INFO("自动启动配置文件大小: {} 字节", fileSize);
  }

  std::vector<std::string> autoStartModules;
  std::optional<boost::json::object> parsedConfigObject;

  std::string raw;
  if (!readFile(configPath, &raw)) {
    bootConfigMode = kBootConfigModeUpper;
    setProcessBootConfigMode(bootConfigMode);
    LOG_ERROR("读取自动启动配置失败: {}，将按安全模式继续执行持久化文件痕迹自动启动", configPath.string());
  } else {
    LOG_INFO("自动启动配置读取完成，实际字节数: {}", raw.size());
    LOG_INFO("自动启动配置原始文本包含 auto_start_modules: {}", raw.find("auto_start_modules") != std::string::npos ? "是" : "否");

    auto json = stripJsonComments(raw);
    LOG_INFO("自动启动配置去注释后文本包含 auto_start_modules: {}", json.find("auto_start_modules") != std::string::npos ? "是" : "否");
    boost::json::value parsed;
    if (!parseJsonValue(json, &parsed, "自动启动配置", "解析配置", metaData_.name, false)) {
      bootConfigMode = kBootConfigModeUpper;
      setProcessBootConfigMode(bootConfigMode);
      LOG_WARNING("自动启动配置解析失败，将按安全模式继续执行持久化文件痕迹自动启动");
    } else if (!parsed.is_object()) {
      bootConfigMode = kBootConfigModeUpper;
      setProcessBootConfigMode(bootConfigMode);
      LOG_ERROR("自动启动配置必须为对象，将按安全模式继续执行持久化文件痕迹自动启动");
    } else {
      parsedConfigObject = parsed.as_object();
      bootConfigMode = parseBootConfigMode(*parsedConfigObject);
      setProcessBootConfigMode(bootConfigMode);
    }
  }

  if (!parsedConfigObject.has_value()) {
    LOG_INFO("自动启动配置未形成可解析对象，本次仅保留 boot_config_mode={} 的安全回退语义", bootConfigMode);
  }

  if (parsedConfigObject.has_value()) {
    const auto &obj = *parsedConfigObject;
    std::vector<std::string> fieldNames;
    fieldNames.reserve(obj.size());
    for (const auto &entry : obj) {
      const auto &key = entry.key();
      fieldNames.emplace_back(key.data(), key.size());
    }
    LOG_INFO("自动启动配置字段: {}", joinNames(fieldNames, ","));
    auto fieldIt = obj.find("auto_start_modules");
    if (fieldIt == obj.end()) {
      LOG_INFO("自动启动配置未包含 auto_start_modules");
      logJsonFieldDetails("自动启动配置", obj, "解析配置", metaData_.name);
      if (obj.empty()) {
        LOG_INFO("自动启动配置字段为空，无法解析任何字段");
      }
    } else if (!fieldIt->value().is_array()) {
      LOG_ERROR("自动启动配置的 auto_start_modules 必须为数组");
      logJsonFieldDetails("自动启动配置", obj, "解析配置", metaData_.name);
    } else {
      const auto &list = fieldIt->value().as_array();
      if (list.empty()) {
        LOG_INFO("自动启动配置的 auto_start_modules 为空");
      } else {
        autoStartModules.reserve(list.size());
        for (const auto &value : list) {
          if (!value.is_string()) {
            LOG_WARNING("自动启动模块条目不是字符串");
            continue;
          }
          const auto &nameValue = value.as_string();
          std::string moduleName(nameValue.c_str(), nameValue.size());
          if (moduleName.empty()) {
            LOG_WARNING("自动启动模块条目为空");
            continue;
          }
          autoStartModules.push_back(std::move(moduleName));
        }
      }
    }
  }

  if (bootConfigMode == kBootConfigModeConfigPusher) {
    clearManagedPersistentTraceFilesForConfigPusher();
  }

  if (!autoStartModules.empty() || bootConfigMode == kBootConfigModeUpper) {
    ensureModuleInfos();
  }

  int started = 0;
  auto tryStartModule = [this, &bootConfigMode, &started](const std::string &moduleName, std::string_view reason, const std::vector<std::filesystem::path> *tracePaths) {
    if (bootConfigMode == kBootConfigModeUpper && moduleName == kConfigPusherModuleName) {
      LOG_INFO("当前 boot_config_mode={}，跳过自动启动模块: {}", bootConfigMode, moduleName);
      return;
    }

    const bool alreadyRunning = isModuleRunning(moduleName);
    if (tracePaths != nullptr) {
      LOG_INFO("当前 boot_config_mode={}，发现模块持久化配置文件痕迹: 模块={}, 文件={}", bootConfigMode, moduleName, joinPaths(*tracePaths, ", "));
      LOG_INFO("当前 boot_config_mode={}，因{}自动启动模块: 模块={}, 文件={}", bootConfigMode, reason, moduleName, joinPaths(*tracePaths, ", "));
    } else {
      LOG_INFO("因{}自动启动模块: {}", reason, moduleName);
    }

    auto result = startModuleByName(moduleName);
    if (!result.ok()) {
      if (tracePaths != nullptr) {
        LOG_ERROR("按持久化配置文件痕迹自动启动模块失败: 模块={}, 文件={}, 原因={}", moduleName, joinPaths(*tracePaths, ", "), result.message);
      } else {
        LOG_ERROR("自动启动模块 {} 失败: {}", moduleName, result.message);
      }
      return;
    }
    if (!alreadyRunning) {
      ++started;
    }
  };

  for (const auto &moduleName : autoStartModules) {
    tryStartModule(moduleName, "auto_start_modules 配置", nullptr);
  }

  if (bootConfigMode == kBootConfigModeUpper) {
    LOG_INFO("当前 boot_config_mode={}，开始按持久化配置文件痕迹自动启动模块（仅检查文件存在性，不预解析 pb 内容）", bootConfigMode);
    const auto modulesByTrace = collectUpperModeAutoStartModulesByTrace();
    if (modulesByTrace.empty()) {
      LOG_INFO("当前 boot_config_mode={}，未发现任何模块持久化配置文件痕迹", bootConfigMode);
    }
    for (const auto &[moduleName, tracePaths] : modulesByTrace) {
      tryStartModule(moduleName, "持久化配置文件痕迹", &tracePaths);
    }
  }

  LOG_INFO("自动启动完成，新增启动 {} 个模块", started);
}
ModuleOpResult ModuleManager::loadModule(ModuleManagerProto::ModuleInfo moduleInfo) {
  ensureModuleInfos();
  std::string moduleName;
  auto resolveResult = resolveModuleName(moduleInfo, &moduleName);
  if (!resolveResult.ok()) {
    return resolveResult;
  }
  return startModuleByName(moduleName);
}
ModuleOpResult ModuleManager::unloadModule(ModuleManagerProto::ModuleInfo moduleInfo) {
  ensureModuleInfos();
  std::string moduleName;
  auto resolveResult = resolveModuleName(moduleInfo, &moduleName);
  if (!resolveResult.ok()) {
    return resolveResult;
  }
  return stopModuleByName(moduleName);
}
ModuleManagerProto::ModuleInfos &ModuleManager::getModuleInfos() {
  ensureModuleInfos();
  return moduleInfos_;
}
ModuleOpResult ModuleManager::resolveModuleName(const ModuleManagerProto::ModuleInfo &moduleInfo, std::string *moduleName) {
  if (moduleName == nullptr) {
    return {ModuleOpError::kInternal, "moduleName 输出参数为空"};
  }
  if (!moduleInfo.module_name().empty()) {
    *moduleName = moduleInfo.module_name();
  } else if (!moduleInfo.lib_name().empty()) {
    *moduleName = extractModuleNameFromLibName(moduleInfo.lib_name());
  }
  if (moduleName->empty()) {
    if (moduleInfo.lib_name().empty()) {
      return {ModuleOpError::kInvalidArgument, "module_name 或 lib_name 不能为空"};
    }
    return {ModuleOpError::kInvalidArgument, "lib_name 非法"};
  }

  auto infoIt = moduleInfoByName_.find(*moduleName);
  if (infoIt == moduleInfoByName_.end()) {
    return {ModuleOpError::kNotFound, "未找到模块: " + *moduleName};
  }
  if (!moduleInfo.lib_name().empty() && moduleInfo.lib_name() != infoIt->second.lib_name()) {
    return {ModuleOpError::kInvalidArgument, "模块 lib_name 不匹配: " + *moduleName};
  }
  return {ModuleOpError::kOk, {}};
}
ModuleOpResult ModuleManager::startModuleByName(const std::string &moduleName) {
  auto infoIt = moduleInfoByName_.find(moduleName);
  if (infoIt == moduleInfoByName_.end()) {
    return {ModuleOpError::kNotFound, "未找到模块: " + moduleName};
  }
  if (!infoIt->second.manifest_error().empty()) {
    return {ModuleOpError::kFailedPrecondition, "模块 manifest_error: " + infoIt->second.manifest_error()};
  }
  if (isModuleRunning(moduleName)) {
    LOG_INFO("模块已在运行，跳过启动: {}", moduleName);
    return {ModuleOpError::kOk, {}};
  }

  std::vector<std::string> order;
  auto resolveResult = resolveStartOrder(moduleName, &order);
  if (!resolveResult.ok()) {
    LOG_ERROR("模块 {} 依赖解析失败: {}", moduleName, resolveResult.message);
    return resolveResult;
  }
  LOG_INFO("启动模块 {}，依赖顺序: {}", moduleName, joinNames(order, " -> "));

  std::vector<std::string> started;
  for (const auto &name : order) {
    if (isModuleRunning(name)) {
      LOG_INFO("模块已在运行，跳过启动: {}", name);
      continue;
    }
    const auto info = moduleInfoByName_.at(name);
    try {
      libInfoVec_.emplace_back(LibInfo::create(info));
    } catch (const std::exception &ex) {
      LOG_ERROR("启动模块 {} 失败: {}", name, ex.what());
      for (auto it = started.rbegin(); it != started.rend(); ++it) {
        if (stopRunningModuleByName(*it)) {
          LOG_INFO("回滚停止模块: {}", *it);
        }
      }
      return {ModuleOpError::kInternal, std::string("启动模块失败: ") + ex.what()};
    }
    started.push_back(name);
    LOG_INFO("模块启动完成: {}", name);
  }
  return {ModuleOpError::kOk, {}};
}
ModuleOpResult ModuleManager::stopModuleByName(const std::string &moduleName) {
  auto infoIt = moduleInfoByName_.find(moduleName);
  if (infoIt == moduleInfoByName_.end()) {
    return {ModuleOpError::kNotFound, "未找到模块: " + moduleName};
  }

  std::vector<std::string> order;
  auto resolveResult = resolveStopOrder(moduleName, &order);
  if (!resolveResult.ok()) {
    LOG_ERROR("模块 {} 级联解析失败: {}", moduleName, resolveResult.message);
    return resolveResult;
  }
  LOG_INFO("停止模块 {}，级联顺序: {}", moduleName, joinNames(order, " -> "));

  for (const auto &name : order) {
    if (!isModuleRunning(name)) {
      LOG_INFO("模块未运行，跳过停止: {}", name);
      continue;
    }
    if (!stopRunningModuleByName(name)) {
      LOG_ERROR("停止模块失败: {}", name);
      return {ModuleOpError::kInternal, "停止模块失败: " + name};
    }
    LOG_INFO("模块停止完成: {}", name);
  }
  return {ModuleOpError::kOk, {}};
}
ModuleOpResult ModuleManager::resolveStartOrder(const std::string &moduleName, std::vector<std::string> *order) {
  if (order == nullptr) {
    return {ModuleOpError::kInternal, "order 输出参数为空"};
  }
  order->clear();

  enum class VisitState {
    kVisiting,
    kVisited,
  };
  std::unordered_map<std::string, VisitState> states;
  std::vector<std::string> stack;
  std::string error;

  std::function<bool(const std::string &)> dfs = [&](const std::string &name) {
    const auto stateIt = states.find(name);
    if (stateIt != states.end()) {
      if (stateIt->second == VisitState::kVisiting) {
        auto cycleStart = std::find(stack.begin(), stack.end(), name);
        std::vector<std::string> cycle(cycleStart, stack.end());
        cycle.push_back(name);
        error = "检测到依赖环: " + joinNames(cycle, " -> ");
        return false;
      }
      return true;
    }

    auto infoIt = moduleInfoByName_.find(name);
    if (infoIt == moduleInfoByName_.end()) {
      error = "依赖模块不存在: " + name;
      return false;
    }
    const auto &info = infoIt->second;
    if (!info.manifest_error().empty()) {
      error = "模块不可用: " + name + " (" + info.manifest_error() + ")";
      return false;
    }

    states.emplace(name, VisitState::kVisiting);
    stack.push_back(name);
    for (const auto &dependency : info.dependencies()) {
      if (dependency.module_name().empty()) {
        error = "模块依赖名为空: " + name;
        return false;
      }
      auto depIt = moduleInfoByName_.find(dependency.module_name());
      if (depIt == moduleInfoByName_.end()) {
        error = "模块 " + name + " 依赖缺失: " + dependency.module_name();
        return false;
      }
      if (!depIt->second.manifest_error().empty()) {
        error = "模块 " + name + " 依赖不可用: " + dependency.module_name() + " (" + depIt->second.manifest_error() + ")";
        return false;
      }

      if (!dependency.version_range().empty()) {
        std::vector<VersionConstraint> constraints;
        std::string parseError;
        if (!parseVersionRange(dependency.version_range(), &constraints, &parseError)) {
          error = "模块 " + name + " 依赖版本约束非法: " + dependency.module_name() + " (" + parseError + ")";
          return false;
        }
        if (depIt->second.version().version().empty()) {
          error = "依赖模块版本缺失: " + dependency.module_name();
          return false;
        }
        std::vector<int> versionParts;
        if (!parseVersionParts(depIt->second.version().version(), &versionParts, &parseError)) {
          error = "依赖模块版本非法: " + dependency.module_name() + " (" + parseError + ")";
          return false;
        }
        if (!isVersionSatisfied(versionParts, constraints)) {
          error = "模块 " + name + " 依赖版本不满足: " + dependency.module_name() + " 约束 " + dependency.version_range() + " 实际 " + depIt->second.version().version();
          return false;
        }
      }

      if (!dfs(dependency.module_name())) {
        return false;
      }
    }
    stack.pop_back();
    states[name] = VisitState::kVisited;
    order->push_back(name);
    return true;
  };

  if (!dfs(moduleName)) {
    return {ModuleOpError::kFailedPrecondition, error};
  }
  return {ModuleOpError::kOk, {}};
}
ModuleOpResult ModuleManager::resolveStopOrder(const std::string &moduleName, std::vector<std::string> *order) {
  if (order == nullptr) {
    return {ModuleOpError::kInternal, "order 输出参数为空"};
  }
  order->clear();

  enum class VisitState {
    kVisiting,
    kVisited,
  };
  std::unordered_map<std::string, VisitState> states;
  std::vector<std::string> stack;
  std::string error;

  std::function<bool(const std::string &)> dfs = [&](const std::string &name) {
    const auto stateIt = states.find(name);
    if (stateIt != states.end()) {
      if (stateIt->second == VisitState::kVisiting) {
        auto cycleStart = std::find(stack.begin(), stack.end(), name);
        std::vector<std::string> cycle(cycleStart, stack.end());
        cycle.push_back(name);
        error = "检测到依赖环: " + joinNames(cycle, " -> ");
        return false;
      }
      return true;
    }
    states.emplace(name, VisitState::kVisiting);
    stack.push_back(name);
    const auto reverseIt = reverseDependencies_.find(name);
    if (reverseIt != reverseDependencies_.end()) {
      for (const auto &dependent : reverseIt->second) {
        if (!dfs(dependent)) {
          return false;
        }
      }
    }
    stack.pop_back();
    states[name] = VisitState::kVisited;
    order->push_back(name);
    return true;
  };

  if (!dfs(moduleName)) {
    return {ModuleOpError::kFailedPrecondition, error};
  }
  return {ModuleOpError::kOk, {}};
}
bool ModuleManager::isModuleRunning(const std::string &moduleName) const {
  return std::any_of(libInfoVec_.begin(), libInfoVec_.end(), [&](const std::shared_ptr<LibInfo> &lib) {
    return lib->MetaData().name == moduleName;
  });
}
bool ModuleManager::stopRunningModuleByName(const std::string &moduleName) {
  auto libInfoIt = std::find_if(libInfoVec_.begin(), libInfoVec_.end(), [&](const std::shared_ptr<LibInfo> &elem) {
    return elem->MetaData().name == moduleName;
  });
  if (libInfoIt == libInfoVec_.end()) {
    return false;
  }
  libInfoIt->get()->cleanUp();
  libInfoVec_.erase(libInfoIt);
  return true;
}
ModuleManagerProto::ModuleRunningInfos ModuleManager::getModuleRunningInfos() {
  ModuleManagerProto::ModuleRunningInfos result;
  for (const auto &lib : libInfoVec_) {
    auto moduleRunningInfo = result.add_module_running_info();
    auto libMetaData = lib->MetaData();
    moduleRunningInfo->set_module_name(libMetaData.name);
    auto version = moduleRunningInfo->mutable_version();
    version->set_major(libMetaData.version.major);
    version->set_minor(libMetaData.version.minor);
    version->set_patch(libMetaData.version.patch);
    version->set_version(libMetaData.version.version);
    moduleRunningInfo->set_lib_name(libMetaData.libName);
    moduleRunningInfo->set_inner_grpc_server(libMetaData.innerGRPCServer);
    moduleRunningInfo->set_outer_grpc_server(libMetaData.outerGRPCServer);
  }
  return result;
}
void ModuleManager::saveModuleStartConfig(ModuleManagerProto::ModuleInfos moduleInfos) {
  std::string configBin("modConf.bin");
  auto confDir = std::filesystem::path("./conf");
  if (!std::filesystem::exists(confDir)) {
    std::filesystem::create_directories(confDir);
  }
  std::ofstream ofs(confDir / configBin, std::ios::binary | std::ios::trunc);
  if (ofs.is_open()) {
    moduleInfos.SerializeToOstream(&ofs);
    moduleConfig_ = moduleInfos;
  }
}
void ModuleManager::initModuleInfos() {
  ModuleInfosBuildGuard buildGuard(moduleInfosBuilding_, "initModuleInfos");
  selfCheckAutoStartConfig("扫描模块前", metaData_.name, true);
  moduleInfos_.Clear();
  moduleInfoByName_.clear();
  reverseDependencies_.clear();
  moduleInfosReady_ = true;
  const std::filesystem::path libDir("./module");
  LOG_INFO("模块目录: {}", libDir.string());
  if (!std::filesystem::exists(libDir) || !std::filesystem::is_directory(libDir)) {
    LOG_WARNING("模块目录不存在或不可访问: {}", libDir.string());
    return;
  }

  int total = 0;
  int available = 0;
  int invalid = 0;
  for (const auto &entry : std::filesystem::directory_iterator(libDir)) {
    if (entry.is_symlink() || !entry.is_regular_file()) {
      continue;
    }
    const std::string fileName = entry.path().filename().string();
    auto moduleName = extractModuleNameFromLibName(fileName);
    if (moduleName.empty()) {
      continue;
    }
    if (moduleName == "moduleManager") {
      LOG_INFO("跳过核心模块库扫描: {}", entry.path().string());
      continue;
    }
    ++total;
    auto moduleInfo = moduleInfos_.add_module_info();
    moduleInfo->set_module_name(moduleName);
    moduleInfo->set_lib_name(fileName);
    LOG_INFO("开始读取模块信息: {}", entry.path().string());

    ModuleManagerProto::ModuleManifest manifest;
    std::string error;
    if (!loadModuleManifest(entry.path(), &manifest, &error, &manifestLibs_)) {
      moduleInfo->set_manifest_error(error);
      ++invalid;
      LOG_ERROR("模块 {} manifest 读取失败: {}", moduleName, error);
    } else if (!validateManifest(manifest, moduleName, &error)) {
      moduleInfo->set_manifest_error(error);
      ++invalid;
      LOG_ERROR("模块 {} manifest 校验失败: {}", moduleName, error);
    } else {
      moduleInfo->mutable_version()->CopyFrom(manifest.version());
      for (const auto &dependency : manifest.dependencies()) {
        moduleInfo->add_dependencies()->CopyFrom(dependency);
      }
      LOG_INFO("模块 {} manifest 读取完成: deps {}", moduleName, manifest.dependencies_size());
      ++available;
      auto fileVersion = parseVersion(fileName);
      if (!fileVersion.version().empty() && fileVersion.version() != manifest.version().version()) {
        LOG_WARNING("模块 {} 版本不一致: manifest {} lib {}", moduleName, manifest.version().version(), fileVersion.version());
      }
    }
    moduleInfoByName_[moduleName] = *moduleInfo;
  }

  for (const auto &entry : moduleInfoByName_) {
    const auto &info = entry.second;
    if (!info.manifest_error().empty()) {
      continue;
    }
    for (const auto &dependency : info.dependencies()) {
      reverseDependencies_[dependency.module_name()].push_back(info.module_name());
    }
  }
  LOG_INFO("模块清单扫描完成: 总数 {}, 可用 {}, 无效 {}", total, available, invalid);
}
ModuleManagerProto::ModuleVersion ModuleManager::parseVersion(std::string libName) {
  ModuleManagerProto::ModuleVersion version;
  auto versionPos = libName.find(".so.");
  if (versionPos == std::string::npos || versionPos + 4 >= libName.size()) {
    return version;
  }
  auto versionStr = libName.substr(versionPos + 4);
  version.set_version(versionStr);

  std::stringstream ss(versionStr);
  std::string segment;
  std::vector<std::string> parts;
  while (std::getline(ss, segment, '.')) {
    parts.emplace_back(segment);
  }
  if (!parts.empty()) {
    version.set_major(parts[0]);
  }
  if (parts.size() > 1) {
    version.set_minor(parts[1]);
  }
  if (parts.size() > 2) {
    version.set_patch(parts[2]);
  }
  return version;
}
}  // namespace ModuleManager
