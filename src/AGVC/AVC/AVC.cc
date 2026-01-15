#include "AVC.h"

#include <boost/dll.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>

#include "AVCGrpcService.h"
#include "AVCLibInfo.h"
#include "Logger.h"
#include "ModuleManager.pb.h"

namespace {
const std::string &GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(AVCLibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(AVCLibInfo.VERSION_MAJOR);
    version->set_minor(AVCLibInfo.VERSION_MINOR);
    version->set_patch(AVCLibInfo.VERSION_PATCH);
    version->set_version(AVCLibInfo.VERSION);
    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace AVC {
AVC::AVC() :
  ModuleInterface(),
  avcService_(std::make_shared<AVCGrpcServiceImpl>()) {
  initLibInfo(AVCLibInfo);
}
AVC::~AVC() {}
void AVC::start(std::stop_token stopToken) {
  LOG_INFO("AVC 模块启动");
  avcService_->getAVC(this);
  grpcServerBuilder(avcService_);
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  LOG_INFO("AVC 模块停止");
}
}  // namespace AVC

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new AVC::AVC();
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
