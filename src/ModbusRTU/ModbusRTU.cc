#include "ModbusRTU.h"

#include <boost/dll.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

#include "ModbusRTUGrpcService.h"
#include "ModuleManager.pb.h"
#include "ModbusRTULibInfo.h"
#include "Logger.h"

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

    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace ModbusRTU {
ModbusRTU::ModbusRTU() :
  ModuleInterface(),
  modbusRTUService_(std::make_shared<ModbusRTUGrpcServiceImpl>()),
  linkManager_(ModbusRTULibInfo.LIB_NAME) {
  initLibInfo(ModbusRTULibInfo);
}
ModbusRTU::~ModbusRTU() {}
void ModbusRTU::start(std::stop_token stopToken) {
  LOG_INFO("ModbusRTU 模块启动");
  modbusRTUService_->setModbusRTU(this);
  LOG_INFO("ModbusRTU 服务实例绑定完成");
  grpcServerBuilder(modbusRTUService_);
  LOG_INFO("ModbusRTU gRPC 服务已启动");

  std::mutex mu;
  std::condition_variable_any cv;
  std::stop_callback cb(stopToken, [&cv]() { cv.notify_all(); });
  std::unique_lock lock(mu);
  cv.wait(lock, [&stopToken]() { return stopToken.stop_requested(); });
  LOG_INFO("ModbusRTU 模块停止");
}

LinkManager& ModbusRTU::linkManager() {
  return linkManager_;
}

const LinkManager& ModbusRTU::linkManager() const {
  return linkManager_;
}
}  // namespace ModbusRTU

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
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
