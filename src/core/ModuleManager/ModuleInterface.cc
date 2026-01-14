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

#include "Logger.h"

namespace ModuleInterface {
std::set<std::string> ModuleInterface::allocatedPorts_;
std::mutex ModuleInterface::portMutex_;
ModuleInterface::ModuleInterface() {
  ModuleManager::Logger::init();
}
ModuleInterface::~ModuleInterface() {
  shutdownServers();
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
  auto sockPath = std::format("unix:{}", absFilePath);
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

  LOG_INFO("模块信息: \nname:\t\t{}\nlibNmae:\t{}\nversion:\t{}\ninner server:\t{}\nouter server:\t{}", metaData_.name, metaData_.libName, metaData_.version.version, metaData_.innerGRPCServer, metaData_.outerGRPCServer);

  innerServerThread_ = std::jthread([this]() {
    if (innerServer_) {
      innerServer_->Wait();
    }
  });
  outerServerThread_ = std::jthread([this]() {
    if (outerServer_) {
      outerServer_->Wait();
    }
  });
}
void ModuleInterface::shutdownServers() {
  if (innerServer_) {
    innerServer_->Shutdown();
  }
  if (outerServer_) {
    outerServer_->Shutdown();
  }
  if (innerServerThread_.joinable()) {
    innerServerThread_.join();
  }
  if (outerServerThread_.joinable()) {
    outerServerThread_.join();
  }
  // 当模块卸载后需要将占用的端口释放
  releasePort(metaData_.outerGRPCServer);
}
void ModuleInterface::releasePort(std::string address) {
  auto pos = address.find(':');
  if (pos == std::string::npos) {
    return;
  }
  auto port = address.substr(pos + 1);
  std::lock_guard<std::mutex> lock(portMutex_);
  allocatedPorts_.erase(port);
}
void ModuleInterface::reservePort(std::string address) {
  auto pos = address.find(':');
  if (pos == std::string::npos) {
    return;
  }
  auto port = address.substr(pos + 1);
  std::lock_guard<std::mutex> lock(portMutex_);
  allocatedPorts_.emplace(port);
}
std::string ModuleInterface::getRandomPort() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dist(7001, 7999);

  while (true) {
    auto port = std::to_string(dist(gen));
    std::lock_guard<std::mutex> lock(portMutex_);
    auto [_, inserted] = allocatedPorts_.emplace(port);
    if (inserted) {
      return port;
    }
  }
}
}  // namespace ModuleInterface
