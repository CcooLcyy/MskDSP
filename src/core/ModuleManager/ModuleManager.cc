#include "ModuleManager.h"

#include <grpcpp/completion_queue.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/service_type.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <thread>
#include <vector>

#include "ModuleInterface.h"
#include "ModuleManager.pb.h"
#include "ModuleManagerGrpcService.h"
#include "moduleManagerLibInfo.h"

namespace ModuleManager {
ModuleManager::ModuleManager(std::stop_source stopSource) :
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