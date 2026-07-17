#include "ModbusRTU.h"

#include <boost/dll.hpp>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

#include "Logger.h"
#include "ModbusRTUGrpcService.h"
#include "ModbusRTULibInfo.h"
#include "ModuleManager.pb.h"

namespace {
const std::string &GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(ModbusRTULibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(ModbusRTULibInfo.VERSION_MAJOR);
    version->set_minor(ModbusRTULibInfo.VERSION_MINOR);
    version->set_patch(ModbusRTULibInfo.VERSION_PATCH);
    version->set_version(ModbusRTULibInfo.VERSION);

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

namespace ModbusRTU {
ModbusRTU::ModbusRTU() :
  ModuleInterface(),
  modbusRTUService_(std::make_shared<ModbusRTUGrpcServiceImpl>()),
  commandService_(std::make_shared<ModbusRTUCommandExecutorServiceImpl>()),
  linkManager_(ModbusRTULibInfo.LIB_NAME) {
  initLibInfo(ModbusRTULibInfo);
}
ModbusRTU::~ModbusRTU() {}
void ModbusRTU::start(std::stop_token stopToken) {
  LOG_INFO("ModbusRTU 模块启动");
  LOG_INFO("ModbusRTU 依赖模块: DataCenter, MQTTManager");
  modbusRTUService_->setModbusRTU(this);
  commandService_->setModbusRTU(this);
  LOG_INFO("ModbusRTU 服务实例绑定完成");
  std::vector<std::shared_ptr<grpc::Service>> services{modbusRTUService_, commandService_};
  grpcServerBuilder(services);
  LOG_INFO("ModbusRTU gRPC 服务已启动");
  linkManager_.LoadPersistedConfig();

  std::mutex mu;
  std::condition_variable_any cv;
  std::stop_callback cb(stopToken, [&cv]() { cv.notify_all(); });
  std::unique_lock lock(mu);
  cv.wait(lock, [&stopToken]() { return stopToken.stop_requested(); });
  LOG_INFO("ModbusRTU 模块停止");
}

LinkManager &ModbusRTU::linkManager() {
  return linkManager_;
}

const LinkManager &ModbusRTU::linkManager() const {
  return linkManager_;
}
}  // namespace ModbusRTU

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface *create() {
  return new ModbusRTU::ModbusRTU();
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
