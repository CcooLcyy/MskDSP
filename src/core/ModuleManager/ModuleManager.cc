#include "ModuleManager.h"

#include <grpcpp/completion_queue.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/service_type.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>

#include <chrono>
#include <stop_token>
#include <thread>
#include <vector>

#include "ModuleInterface.h"
#include "ModuleManagerGrpcService.h"
#include "moduleManagerLibInfo.h"

namespace ModuleManager {
ModuleManager::ModuleManager(std::stop_source stopSource) :
  ModuleInterface::ModuleInterface(stopSource) {
  initLibInfo(moduleManagerLibInfo);
  metaData_.outerGRPCServer = std::string("0.0.0.0:7000");
}
ModuleManager::~ModuleManager() {
  stopGrpcServer();
}
::ModuleInterface::MetaData ModuleManager::metaData() {
  return metaData_;
}
void ModuleManager::start() {
  ServiceImpl service;
  std::vector<grpc::Service *> services;
  services.emplace_back(&service);
  std::jthread innerServerThread([&]() { runInnerServer(services); });
  std::jthread outerServerThread([&]() { runOuterServer(services); });
  while (!stopToken_.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
ModuleManagerProto::ModuleInfos &ModuleManager::getModuleInfo() {
  return moduleInfos_;
}
}  // namespace ModuleManager