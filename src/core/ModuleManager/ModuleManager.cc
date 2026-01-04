#include "ModuleManager.h"

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
#include <thread>
#include <vector>

#include "Logger.h"
#include "ModuleInterface.h"
#include "ModuleManager.pb.h"
#include "ModuleManagerGrpcService.h"
#include "moduleManagerLibInfo.h"

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
  LOG_INFO("正在启动模块管理器");
  moduleManagerService_->getModuleManager(this);
  grpcServerBuilder(moduleManagerService_);
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
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
  LOG_INFO("初始化模块信息");
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
  LOG_INFO("模块信息初始化结束");
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
