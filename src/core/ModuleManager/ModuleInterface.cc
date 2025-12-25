#include "ModuleInterface.h"

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <stop_token>
#include <string>

#include "moduleManagerLibInfo.h"

namespace ModuleInterface {
ModuleInterface::ModuleInterface(std::stop_source stopSource) :
  stopSource_(stopSource) {
  stopToken_ = stopSource_.get_token();
}
ModuleInterface::~ModuleInterface() {}
void ModuleInterface::stop() {
  stopGrpcServer();
  stopSource_.request_stop();
}
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

  metaData_.outerGRPCServer = std::string("0.0.0.0") + std::to_string(getRandomPort());
}
void ModuleInterface::stopGrpcServer() {
  innerServer_->Shutdown();
  outerServer_->Shutdown();
}
void ModuleInterface::runInnerServer(std::vector<grpc::Service *> innerServices) {
  grpcServerBuilder(innerServices, metaData_.innerGRPCServer, innerServer_);
}
void ModuleInterface::runOuterServer(std::vector<grpc::Service *> outerServices) {
  grpcServerBuilder(outerServices, metaData_.outerGRPCServer, outerServer_);
}
void ModuleInterface::grpcServerBuilder(std::vector<grpc::Service *> services, std::string grpcServer, std::unique_ptr<grpc::Server> &server) {
  grpc::ServerBuilder builder;
  for (auto service : services) {
    builder.RegisterService(service);
  }
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  builder.AddListeningPort(grpcServer, grpc::InsecureServerCredentials());
  std::unique_ptr<grpc::Server> tmpServer(builder.BuildAndStart());
  server = std::move(tmpServer);

  server->Wait();
}
int ModuleInterface::getRandomPort() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> distrib(7001, 7999);
  return distrib(gen);
}
}  // namespace ModuleInterface