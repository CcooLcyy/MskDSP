#include "ModuleManager.h"

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <grpcpp/completion_queue.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/service_type.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>

#include <algorithm>
#include <boost/dll/shared_library.hpp>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

#include "Logger.h"
#include "ModuleInterface.h"
#include "ModuleManager.pb.h"
#include "ModuleManagerGrpcService.h"
#include "moduleManagerLibInfo.h"

namespace {
constexpr const char *kAutoStartConfigPath = "./conf/module_manager.jsonc";

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

bool loadModuleManifest(const std::filesystem::path &libPath, ModuleManagerProto::ModuleManifest *manifest, std::string *error) {
  if (manifest == nullptr) {
    return false;
  }
  LOG_INFO("读取模块 manifest 开始: {}", libPath.string());
  try {
    boost::dll::shared_library lib;
    LOG_INFO("加载模块库: {}", libPath.string());
    lib.load(libPath.string(), boost::dll::load_mode::rtld_lazy);
    LOG_INFO("模块库加载完成: {}", libPath.filename().string());
    LOG_INFO("检查 manifest 符号: {}", libPath.filename().string());
    if (!lib.has("GetModuleManifestPb")) {
      if (error != nullptr) {
        *error = "未找到 manifest 符号 GetModuleManifestPb";
      }
      return false;
    }
    using ManifestFn = bool(const uint8_t **, size_t *);
    auto &fn = lib.get<ManifestFn>("GetModuleManifestPb");
    LOG_INFO("manifest 符号解析成功: {}", libPath.filename().string());
    LOG_INFO("调用 manifest 函数: {}", libPath.filename().string());
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
  metaData_.outerGRPCServer = std::string("0.0.0.0:7000");
  reservePort(metaData_.outerGRPCServer);
}
ModuleManager::~ModuleManager() {}
void ModuleManager::start(std::stop_token stopToken) {
  LogModuleScope moduleScope(metaData_.name);
  LOG_INFO("正在启动模块管理器");
  moduleManagerService_->getModuleManager(this);
  grpcServerBuilder(moduleManagerService_);
  initModuleInfos();
  autoStartModulesFromConfig();
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
void ModuleManager::ensureModuleInfos() {
  if (!moduleInfosReady_) {
    initModuleInfos();
  }
}
void ModuleManager::autoStartModulesFromConfig() {
  const std::filesystem::path configPath(kAutoStartConfigPath);
  if (!std::filesystem::exists(configPath)) {
    LOG_INFO("未找到自动启动配置文件: {}", configPath.string());
    return;
  }

  std::string raw;
  if (!readFile(configPath, &raw)) {
    LOG_ERROR("读取自动启动配置失败: {}", configPath.string());
    return;
  }

  auto json = stripJsonComments(raw);
  google::protobuf::Struct config;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;
  auto status = google::protobuf::util::JsonStringToMessage(json, &config, options);
  if (!status.ok()) {
    LOG_ERROR("解析自动启动配置失败: {}", status.ToString());
    return;
  }

  auto fieldIt = config.fields().find("auto_start_modules");
  if (fieldIt == config.fields().end()) {
    LOG_INFO("自动启动配置未包含 auto_start_modules");
    return;
  }
  if (fieldIt->second.kind_case() != google::protobuf::Value::kListValue) {
    LOG_ERROR("自动启动配置的 auto_start_modules 必须为数组");
    return;
  }

  const auto &list = fieldIt->second.list_value();
  if (list.values().empty()) {
    LOG_INFO("自动启动配置的 auto_start_modules 为空");
    return;
  }

  ensureModuleInfos();
  int started = 0;
  for (const auto &value : list.values()) {
    if (value.kind_case() != google::protobuf::Value::kStringValue) {
      LOG_WARNING("自动启动模块条目不是字符串");
      continue;
    }
    const auto &moduleName = value.string_value();
    if (moduleName.empty()) {
      LOG_WARNING("自动启动模块条目为空");
      continue;
    }
    LOG_INFO("自动启动模块: {}", moduleName);
    auto result = startModuleByName(moduleName);
    if (!result.ok()) {
      LOG_ERROR("自动启动模块 {} 失败: {}", moduleName, result.message);
      continue;
    }
    ++started;
  }

  LOG_INFO("自动启动完成，已启动 {} 个模块", started);
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
  moduleInfos_.Clear();
  moduleInfoByName_.clear();
  reverseDependencies_.clear();
  moduleInfosReady_ = true;
  const std::filesystem::path libDir("./lib");
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
    if (!loadModuleManifest(entry.path(), &manifest, &error)) {
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
