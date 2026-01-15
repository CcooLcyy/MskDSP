#include "ModbusRTU.h"

#include <boost/dll.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
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
  modbusRTUService_(std::make_shared<ModbusRTUGrpcServiceImpl>()) {
  initLibInfo(ModbusRTULibInfo);
}
ModbusRTU::~ModbusRTU() {}
void ModbusRTU::start(std::stop_token stopToken) {
  LOG_INFO("ModbusRTU 模块启动");
  modbusRTUService_->setModbusRTU(this);
  LOG_INFO("ModbusRTU 服务实例绑定完成");
  grpcServerBuilder(modbusRTUService_);
  LOG_INFO("ModbusRTU gRPC 服务已启动");
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  LOG_INFO("ModbusRTU 模块停止");
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
