#include "ModuleInterface.h"

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <string>
#include <thread>

namespace ModuleInterface {
ModuleInterface::ModuleInterface() {}
ModuleInterface::~ModuleInterface() {
  // 当模块卸载后需要将占用的端口释放
  releasePort(metaData_.outerGRPCServer);
}
MetaData ModuleInterface::metaData() {
  return metaData_;
}
void ModuleInterface::initLibInfo(LibInfo libInfo) {
  metaData_.name = libInfo.LIB_NAME;
  metaData_.libName = std::format("{}{}{}.{}", "lib", libInfo.LIB_NAME, ".so", libInfo.VERSION);
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

  auto port = getRandomPort();
  metaData_.outerGRPCServer = std::format("0.0.0.0:{}", port);
}
void ModuleInterface::grpcServerBuilder(std::shared_ptr<grpc::Service> service) {
  grpc::ServerBuilder innerServerbuilder;
  innerServerbuilder.RegisterService(service.get());
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  innerServerbuilder.AddListeningPort(metaData_.innerGRPCServer, grpc::InsecureServerCredentials());
  std::unique_ptr<grpc::Server> innerTmpServer(innerServerbuilder.BuildAndStart());
  innerServer_ = std::move(innerTmpServer);

  grpc::ServerBuilder outerServerBuilder;
  outerServerBuilder.RegisterService(service.get());
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  outerServerBuilder.AddListeningPort(metaData_.outerGRPCServer, grpc::InsecureServerCredentials());
  std::unique_ptr<grpc::Server> outerTmpServer(outerServerBuilder.BuildAndStart());
  outerServer_ = std::move(outerTmpServer);

  std::jthread([&]() { innerServer_->Wait(); }).detach();
  std::jthread([&]() { outerServer_->Wait(); }).detach();
}
void ModuleInterface::releasePort(std::string address) {
  auto port = address.substr(address.find(':') + 1);
  portSet_.erase(port);
}
std::string ModuleInterface::getRandomPort() {
  std::string port;
  auto genRandomPort = [&]() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(7001, 7999);
    port = std::to_string(dist(gen));
  };
  genRandomPort();
  while (isSamePort(port)) {
    genRandomPort();
  }
  portSet_.emplace(port);
  return port;
}
bool ModuleInterface::isSamePort(std::string port) {
  auto portIt = portSet_.find(port);
  return portIt == portSet_.end() ? false : true;
}
}  // namespace ModuleInterface