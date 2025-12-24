#include "ModuleManager.h"

#include <grpcpp/completion_queue.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>

#include <filesystem>
#include <format>
#include <memory>
#include <stop_token>

#include "ModuleInterface.h"
#include "ModuleManagerGrpcService.h"
#include "moduleManagerLibInfo.h"

namespace ModuleManager {
ModuleManager::ModuleManager(std::stop_token stopToken) :
  ModuleInterface::ModuleInterface(stopToken) {
    initLibInfo(moduleManagerLibInfo);
}
ModuleManager::~ModuleManager() {}
::ModuleInterface::MetaData ModuleManager::metaData() {
  return metaData_;
}
void ModuleManager::start() {
  runServer();
}
void ModuleManager::runServer() {
  ServiceImpl service;
  grpc::ServerBuilder builder;
  builder.AddListeningPort(metaData_.grpcServer, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  server->Wait();
}
void ModuleManager::loadModule() {}
}  // namespace ModuleManager