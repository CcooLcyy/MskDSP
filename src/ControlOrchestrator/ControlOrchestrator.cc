#include "ControlOrchestrator.h"

#include <boost/dll.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

#include "ControlOrchestratorGrpcService.h"
#include "ControlOrchestratorLibInfo.h"
#include "Logger.h"
#include "ModuleManager.pb.h"

namespace {
const std::string &GetSerializedManifest() {
  static const std::string serialized = [] {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(ControlOrchestratorLibInfo.LIB_NAME);
    auto *version = manifest.mutable_version();
    version->set_major(ControlOrchestratorLibInfo.VERSION_MAJOR);
    version->set_minor(ControlOrchestratorLibInfo.VERSION_MINOR);
    version->set_patch(ControlOrchestratorLibInfo.VERSION_PATCH);
    version->set_version(ControlOrchestratorLibInfo.VERSION);
    auto *dependency = manifest.add_dependencies();
    dependency->set_module_name("DataCenter");
    dependency->set_version_range("=0.0.1");
    return manifest.SerializeAsString();
  }();
  return serialized;
}
}  // namespace

namespace ControlOrchestrator {

ControlOrchestrator::ControlOrchestrator() :
  ModuleInterface(),
  service_(std::make_shared<GrpcServiceImpl>()),
  commandService_(std::make_shared<CommandExecutorGrpcServiceImpl>()),
  manager_("./conf/config.db") {
  initLibInfo(ControlOrchestratorLibInfo);
}

ControlOrchestrator::~ControlOrchestrator() = default;

void ControlOrchestrator::start(std::stop_token stopToken) {
  LOG_INFO("ControlOrchestrator 模块启动");
  LOG_INFO("ControlOrchestrator 依赖模块: DataCenter");
  service_->setManager(&manager_);
  commandService_->setManager(&manager_);
  grpcServerBuilder(std::vector<std::shared_ptr<grpc::Service>>{service_, commandService_});
  auto status = manager_.LoadPersistedConfig();
  if (!status.ok()) {
    LOG_ERROR("ControlOrchestrator 恢复配置失败: {}", status.error_message());
  }
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  LOG_INFO("ControlOrchestrator 模块停止");
}

SequenceManager &ControlOrchestrator::sequenceManager() {
  return manager_;
}

}  // namespace ControlOrchestrator

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface *create() {
  return new ControlOrchestrator::ControlOrchestrator();
}

extern "C" BOOST_SYMBOL_EXPORT bool GetModuleManifestPb(const uint8_t **data, size_t *size) {
  if (data == nullptr || size == nullptr) {
    return false;
  }
  const auto &serialized = GetSerializedManifest();
  *data = reinterpret_cast<const uint8_t *>(serialized.data());
  *size = serialized.size();
  return true;
}
