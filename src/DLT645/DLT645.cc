#include "DLT645.h"

#include <boost/dll.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>

#include "DLT645GrpcService.h"
#include "DLT645LibInfo.h"
#include "Logger.h"
#include "ModuleManager.pb.h"

namespace {
const std::string &GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(DLT645LibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(DLT645LibInfo.VERSION_MAJOR);
    version->set_minor(DLT645LibInfo.VERSION_MINOR);
    version->set_patch(DLT645LibInfo.VERSION_PATCH);
    version->set_version(DLT645LibInfo.VERSION);

    auto dependency = manifest.add_dependencies();
    dependency->set_module_name("DataCenter");
    dependency->set_version_range("=0.0.1");

    auto mqttDependency = manifest.add_dependencies();
    mqttDependency->set_module_name("MQTTManager");
    mqttDependency->set_version_range("=0.0.1");

    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace DLT645 {
DLT645::DLT645() :
  ModuleInterface(),
  dlt645Service_(std::make_shared<DLT645GrpcServiceImpl>()),
  linkManager_(DLT645LibInfo.LIB_NAME) {
  initLibInfo(DLT645LibInfo);
}
DLT645::~DLT645() {}
void DLT645::start(std::stop_token stopToken) {
  LOG_INFO("DLT645 模块启动");
  dlt645Service_->getDLT645(this);
  LOG_INFO("DLT645 服务实例绑定完成");
  grpcServerBuilder(dlt645Service_);
  LOG_INFO("DLT645 gRPC 服务已启动");
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  LOG_INFO("DLT645 模块停止");
}

LinkManager& DLT645::linkManager() {
  return linkManager_;
}

const LinkManager& DLT645::linkManager() const {
  return linkManager_;
}
}  // namespace DLT645

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new DLT645::DLT645();
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
