#include "DataCenter.h"

#include <boost/dll.hpp>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>

#include "DataCenterGrpcService.h"
#include "DataCenterLibInfo.h"
#include "Logger.h"
#include "ModuleManager.pb.h"

namespace {
const std::string &GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(DataCenterLibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(DataCenterLibInfo.VERSION_MAJOR);
    version->set_minor(DataCenterLibInfo.VERSION_MINOR);
    version->set_patch(DataCenterLibInfo.VERSION_PATCH);
    version->set_version(DataCenterLibInfo.VERSION);
    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace DataCenter {
DataCenter::DataCenter() :
  ModuleInterface(),
  dataCenterService_(std::make_shared<DataCenterGrpcServiceImpl>()) {
  initLibInfo(DataCenterLibInfo);
}
DataCenter::~DataCenter() {}
void DataCenter::start(std::stop_token stopToken) {
  LOG_INFO("DataCenter 模块启动");
  grpcServerBuilder(dataCenterService_);

  std::mutex mu;
  std::condition_variable_any cv;
  std::stop_callback cb(stopToken, [&cv]() { cv.notify_all(); });

  std::unique_lock lock(mu);
  cv.wait(lock, [&stopToken]() { return stopToken.stop_requested(); });
  LOG_INFO("DataCenter 模块停止");
}
}  // namespace DataCenter

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new DataCenter::DataCenter();
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
