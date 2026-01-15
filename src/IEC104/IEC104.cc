#include "IEC104.h"

#include <boost/dll.hpp>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "IEC104GrpcService.h"
#include "IEC104LibInfo.h"
#include "Logger.h"
#include "ModuleManager.pb.h"

namespace {
const std::string &GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(IEC104LibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(IEC104LibInfo.VERSION_MAJOR);
    version->set_minor(IEC104LibInfo.VERSION_MINOR);
    version->set_patch(IEC104LibInfo.VERSION_PATCH);
    version->set_version(IEC104LibInfo.VERSION);

    auto dependency = manifest.add_dependencies();
    dependency->set_module_name("DataCenter");
    dependency->set_version_range("=0.0.1");

    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace IEC104 {
IEC104::IEC104() :
  ModuleInterface(),
  iec104Service_(std::make_shared<IEC104GrpcServiceImpl>()),
  linkManager_(IEC104LibInfo.LIB_NAME) {
  initLibInfo(IEC104LibInfo);
}
IEC104::~IEC104() {}
void IEC104::start(std::stop_token stopToken) {
  LOG_INFO("IEC104 模块启动");
  iec104Service_->getIEC104(this);
  grpcServerBuilder(iec104Service_);

  std::mutex mu;
  std::condition_variable_any cv;
  std::stop_callback cb(stopToken, [&cv]() { cv.notify_all(); });
  std::unique_lock lock(mu);
  cv.wait(lock, [&stopToken]() { return stopToken.stop_requested(); });
  LOG_INFO("IEC104 模块停止");
}

LinkManager &IEC104::linkManager() {
  return linkManager_;
}

const LinkManager &IEC104::linkManager() const {
  return linkManager_;
}
}  // namespace IEC104

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface *create() {
  return new IEC104::IEC104();
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
