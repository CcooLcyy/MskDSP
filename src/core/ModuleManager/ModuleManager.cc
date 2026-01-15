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
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
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
  autoStartModulesFromConfig();
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
void ModuleManager::autoStartModulesFromConfig() {
  const std::filesystem::path configPath(kAutoStartConfigPath);
  if (!std::filesystem::exists(configPath)) {
    LOG_INFO("Auto-start config not found: {}", configPath.string());
    return;
  }

  std::string raw;
  if (!readFile(configPath, &raw)) {
    LOG_ERROR("Auto-start config read failed: {}", configPath.string());
    return;
  }

  auto json = stripJsonComments(raw);
  google::protobuf::Struct config;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;
  auto status = google::protobuf::util::JsonStringToMessage(json, &config, options);
  if (!status.ok()) {
    LOG_ERROR("Auto-start config parse failed: {}", status.ToString());
    return;
  }

  auto fieldIt = config.fields().find("auto_start_modules");
  if (fieldIt == config.fields().end()) {
    LOG_INFO("Auto-start config has no auto_start_modules");
    return;
  }
  if (fieldIt->second.kind_case() != google::protobuf::Value::kListValue) {
    LOG_ERROR("Auto-start config auto_start_modules should be array");
    return;
  }

  const auto &list = fieldIt->second.list_value();
  if (list.values().empty()) {
    LOG_INFO("Auto-start config auto_start_modules is empty");
    return;
  }

  initModuleInfos();
  int started = 0;
  for (const auto &value : list.values()) {
    if (value.kind_case() != google::protobuf::Value::kStringValue) {
      LOG_WARNING("Auto-start module entry is not string");
      continue;
    }
    const auto &moduleName = value.string_value();
    if (moduleName.empty()) {
      LOG_WARNING("Auto-start module entry is empty");
      continue;
    }
    auto runningIt = std::find_if(libInfoVec_.begin(), libInfoVec_.end(), [&](const std::shared_ptr<LibInfo> &lib) {
      return lib->MetaData().name == moduleName;
    });
    if (runningIt != libInfoVec_.end()) {
      LOG_INFO("Auto-start skip, module already running: {}", moduleName);
      continue;
    }

    auto moduleInfoIt = std::find_if(moduleInfos_.module_info().begin(), moduleInfos_.module_info().end(), [&](const ModuleManagerProto::ModuleInfo &elem) {
      return elem.module_name() == moduleName;
    });
    if (moduleInfoIt == moduleInfos_.module_info().end()) {
      LOG_WARNING("Auto-start module not found in ./lib: {}", moduleName);
      continue;
    }

    LOG_INFO("Auto-start module: {}", moduleName);
    libInfoVec_.emplace_back(LibInfo::create(*moduleInfoIt));
    ++started;
  }

  LOG_INFO("Auto-start completed, started {}", started);
}
void ModuleManager::loadModule(ModuleManagerProto::ModuleInfo moduleInfo) {
  initModuleInfos();
  auto moduleInfoIt = std::find_if(moduleInfos_.module_info().begin(), moduleInfos_.module_info().end(), [&](const ModuleManagerProto::ModuleInfo &elem) {
    return elem.module_name() == moduleInfo.module_name();
  });
  if (moduleInfoIt != moduleInfos_.module_info().end()) {
    libInfoVec_.emplace_back(LibInfo::create(moduleInfo));
  }
}
void ModuleManager::unloadModule(ModuleManagerProto::ModuleInfo moduleInfo) {
  auto libInfoIt = std::find_if(libInfoVec_.begin(), libInfoVec_.end(), [&](const std::shared_ptr<LibInfo> &elem) {
    return elem->MetaData().libName == moduleInfo.lib_name();
  });
  if (libInfoIt != libInfoVec_.end()) {
    libInfoIt->get()->cleanUp();
    libInfoVec_.erase(libInfoIt);
  }
}
ModuleManagerProto::ModuleInfos &ModuleManager::getModuleInfos() {
  initModuleInfos();
  return moduleInfos_;
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
  const std::filesystem::path libDir("./lib");
  if (!std::filesystem::exists(libDir) || !std::filesystem::is_directory(libDir)) {
    return;
  }

  for (const auto &entry : std::filesystem::directory_iterator(libDir)) {
    if (entry.is_symlink() || !entry.is_regular_file()) {
      continue;
    }
    const std::string fileName = entry.path().filename().string();
    auto soPos = fileName.find(".so");
    if (soPos == std::string::npos || soPos <= 3) {
      continue;
    }
    auto moduleInfo = moduleInfos_.add_module_info();
    auto moduleName = fileName.substr(3, soPos - 3);
    moduleInfo->set_module_name(moduleName);
    moduleInfo->set_lib_name(fileName);
    moduleInfo->mutable_version()->CopyFrom(parseVersion(fileName));
  }
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
