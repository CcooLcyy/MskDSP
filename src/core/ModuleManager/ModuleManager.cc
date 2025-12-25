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
#include <utility>
#include <vector>

#include "ModuleInterface.h"
#include "ModuleManager.pb.h"
#include "ModuleManagerGrpcService.h"
#include "moduleManagerLibInfo.h"

namespace ModuleManager {
ModuleManager::ModuleManager(std::shared_ptr<std::stop_source> stopSource) :
  ModuleInterface::ModuleInterface(stopSource) {
  initLibInfo(moduleManagerLibInfo);
  metaData_.outerGRPCServer = std::string("0.0.0.0:7000");
  initModuleInfos();
}
ModuleManager::~ModuleManager() {}
void ModuleManager::start() {
  ServiceImpl service;
  service.getModuleManager(this);
  std::vector<grpc::Service *> services;
  services.emplace_back(&service);
  std::jthread innerServerThread([&]() { runInnerServer(services); });
  std::jthread outerServerThread([&]() { runOuterServer(services); });
  while (!stopToken_.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
void ModuleManager::loadModule(ModuleManagerProto::ModuleInfo moduleInfo) {
  boost::dll::shared_library lib(std::string("./lib/") + moduleInfo.lib_name());
  if (!lib.has("create")) {
    lib.unload();
    return;
  }
  auto create = lib.get<ModuleInterface *(void *)>("create");
  std::shared_ptr<std::stop_source> stopSource{std::make_shared<std::stop_source>()};
  std::shared_ptr<ModuleInterface> instance(create(stopSource.get()));
  LibInfo libInfo;
  libInfo.metaData = instance->metaData();
  libInfo.lib = std::move(lib);
  libInfo.instance = instance;
  libInfo.stopSource = stopSource;
  libInfo.thread = std::move(std::jthread([&]() { instance->start(); }));
  libInfoVec_.emplace_back(std::move(libInfo));
}
void ModuleManager::unloadModule(ModuleManagerProto::ModuleInfo moduleInfo) {
  auto libInfoIt = std::find_if(libInfoVec_.begin(), libInfoVec_.end(), [&](const LibInfo &elem) {
    return elem.metaData.libName == moduleInfo.lib_name();
  });
  if (libInfoIt != libInfoVec_.end()) {
    libInfoIt->stopSource->request_stop();
    libInfoVec_.erase(libInfoIt);
  }
}
ModuleManagerProto::ModuleInfos &ModuleManager::getModuleInfos() {
  return moduleInfos_;
}
void ModuleManager::initModuleInfos() {
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