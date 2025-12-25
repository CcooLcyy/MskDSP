#include "ModuleInterface.h"

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <filesystem>
#include <format>
#include <memory>
#include <thread>

#include "moduleManagerLibInfo.h"

namespace ModuleInterface {
ModuleInterface::ModuleInterface(std::stop_token stopToken) {}
void ModuleInterface::initLibInfo(LibInfo libInfo) {
  metaData_.name = libInfo.LIB_NAME;
  metaData_.libName = std::format("{}{}{}", "lib", libInfo.LIB_NAME, ".so");
  ::ModuleInterface::Version versionInfo{
      libInfo.VERSION_MAJOR,
      libInfo.VERSION_MINOR,
      libInfo.VERSION_PATCH,
      libInfo.VERSION};
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
  metaData_.innerGRPCServer = sockPath;

  // metaData_.outterGRPCServer = 
}
void ModuleInterface::stopGrpcServer() {
  innerServer_->Shutdown();
  outerServer_->Shutdown();
}
void ModuleInterface::runInnerServer(std::vector<grpc::Service *> innerServices) {
  grpcServerBuilder(innerServices, metaData_.innerGRPCServer, innerServer_);
}
void ModuleInterface::runOuterServer(std::vector<grpc::Service *> outerServices) {
  grpcServerBuilder(outerServices, metaData_.outterGRPCServer, outerServer_);
}
void ModuleInterface::grpcServerBuilder(std::vector<grpc::Service *> services, std::string grpcServer, std::unique_ptr<grpc::Server> &server) {
  grpc::ServerBuilder builder;
  for (auto service : services) {
    builder.RegisterService(service);
  }
  builder.AddListeningPort(grpcServer, grpc::InsecureServerCredentials());
  std::unique_ptr<grpc::Server> tmpServer(builder.BuildAndStart());
  server = std::move(tmpServer);

  server->Wait();
}
}  // namespace ModuleInterface