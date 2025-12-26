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
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

#include "ModuleInterface.h"
#include "ModuleManager.pb.h"
#include "ModuleManagerGrpcService.h"
#include "moduleManagerLibInfo.h"

namespace ModuleManager {
ModuleManager::ModuleManager() :
  moduleManagerService_(std::make_shared<ModuleManagerServiceImpl>()),
  ModuleInterface::ModuleInterface() {
  initLibInfo(moduleManagerLibInfo);
  metaData_.outerGRPCServer = std::string("0.0.0.0:7000");
}
ModuleManager::~ModuleManager() {}
void ModuleManager::start(std::stop_token stopToken) {
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
void ModuleManager::initModuleInfos() {
  moduleInfos_.Clear();
  for (auto entry : std::filesystem::directory_iterator("./lib")) {
    if (!entry.is_symlink()) {
      auto moduleInfo = moduleInfos_.add_module_info();
      std::string fileName = entry.path().filename();
      std::string moduleName = fileName.substr(3, fileName.find(".so") - 3);
      moduleInfo->set_module_name(moduleName);
      moduleInfo->set_lib_name(fileName);
      moduleInfo->mutable_version()->CopyFrom(parseVersion(fileName));
    }
  }
}
ModuleManagerProto::ModuleVersion ModuleManager::parseVersion(std::string libName) {
  ModuleManagerProto::ModuleVersion version;
  version.set_version(libName.substr(libName.size() - 5, 5));
  version.set_major(version.version().substr(0, 1));
  version.set_minor(version.version().substr(2, 1));
  version.set_patch(version.version().substr(4, 1));
  return version;
}
}  // namespace ModuleManager