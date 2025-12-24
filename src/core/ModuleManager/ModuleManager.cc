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
  metaData_.name = moduleManagerLibInfo::LIB_NAME;
  metaData_.libName = std::format("{}{}{}", "lib", moduleManagerLibInfo::LIB_NAME, ".so");
  ::ModuleInterface::Version versionInfo{
      moduleManagerLibInfo::VERSION_MAJOR,
      moduleManagerLibInfo::VERSION_MINOR,
      moduleManagerLibInfo::VERSION_PATCH,
      moduleManagerLibInfo::VERSION};
  metaData_.version = versionInfo;
  std::string socktPath{std::format("./socket/{}.sock", metaData_.name)};
  std::filesystem::path path(socktPath);
  if (!std::filesystem::exists(path.parent_path())) {
    std::filesystem::create_directory(path.parent_path());
  }
  auto absPath = std::filesystem::canonical(path.parent_path());
  auto absFilePath = std::format("{}/{}.sock", absPath.c_str(), metaData_.name);
  auto sockPath = std::format("unix:/{}", absFilePath);
  if (std::filesystem::exists(absFilePath)) {
    std::filesystem::remove(absFilePath);
  }
  metaData_.grpcServer = sockPath;
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