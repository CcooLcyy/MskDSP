#include "AGC.h"

#include <boost/dll.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>

#include "AGCGrpcService.h"
#include "AGCLibInfo.h"
#include "Logger.h"
#include "ModuleManager.pb.h"

namespace {
const std::string &GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(AGCLibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(AGCLibInfo.VERSION_MAJOR);
    version->set_minor(AGCLibInfo.VERSION_MINOR);
    version->set_patch(AGCLibInfo.VERSION_PATCH);
    version->set_version(AGCLibInfo.VERSION);

    auto depDataCenter = manifest.add_dependencies();
    depDataCenter->set_module_name("DataCenter");
    depDataCenter->set_version_range("=0.0.1");

    auto depIEC104 = manifest.add_dependencies();
    depIEC104->set_module_name("IEC104");
    depIEC104->set_version_range("=0.0.1");

    auto depModbusRTU = manifest.add_dependencies();
    depModbusRTU->set_module_name("ModbusRTU");
    depModbusRTU->set_version_range("=0.0.1");

    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace AGC {
AGC::AGC() :
  ModuleInterface(),
  agcService_(std::make_shared<AGCGrpcServiceImpl>()),
  groupManager_(AGCLibInfo.LIB_NAME) {
  initLibInfo(AGCLibInfo);
}
AGC::~AGC() {}
void AGC::start(std::stop_token stopToken) {
  LOG_INFO("AGC 模块启动");
  LOG_INFO("AGC 依赖模块: DataCenter, IEC104, ModbusRTU");
  agcService_->getAGC(this);
  grpcServerBuilder(agcService_);
  LOG_INFO("AGC 开始在模块启动阶段恢复本地控制组配置");
  auto restoreStatus = groupManager_.LoadPersistedConfig();
  if (!restoreStatus.ok()) {
    LOG_ERROR("AGC 恢复本地控制组配置存在错误: {}", restoreStatus.error_message());
  }
  LOG_INFO("AGC 开始在模块启动阶段检查已恢复控制组是否满足自动启动条件");
  groupManager_.TryAutoStartReadyGroups("模块启动后恢复检查");
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  LOG_INFO("AGC 模块停止");
}

GroupManager &AGC::groupManager() {
  return groupManager_;
}

const GroupManager &AGC::groupManager() const {
  return groupManager_;
}
}  // namespace AGC

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface *create() {
  return new AGC::AGC();
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
