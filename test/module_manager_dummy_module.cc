#include <boost/dll.hpp>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>

#include "ModuleManager.pb.h"
#include "ModuleInterface.h"

namespace {
const LibInfo kDummyLibInfo{
    .VERSION_MAJOR = "0",
    .VERSION_MINOR = "0",
    .VERSION_PATCH = "1",
    .VERSION = "0.0.1",
    .LIB_NAME = "Dummy",
};

// 供单元测试使用的最小模块实现。它不会主动启动任何 gRPC 服务，
// 以保持测试稳定且执行迅速。
class DummyModule final : public ModuleInterface::ModuleInterface {
public:
  DummyModule() {
    // 保持元数据与生成出的共享库文件名一致，
    // 这样 ModuleManager 才能按 lib_name 正确加载和卸载。
    initLibInfo(kDummyLibInfo);
  }

  void start(std::stop_token stopToken) override {
    std::mutex mu;
    std::condition_variable_any cv;
    std::stop_callback cb(stopToken, [&cv]() { cv.notify_all(); });

    std::unique_lock lock(mu);
    cv.wait(lock, [&stopToken]() { return stopToken.stop_requested(); });
  }
};

const std::string &GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(kDummyLibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(kDummyLibInfo.VERSION_MAJOR);
    version->set_minor(kDummyLibInfo.VERSION_MINOR);
    version->set_patch(kDummyLibInfo.VERSION_PATCH);
    version->set_version(kDummyLibInfo.VERSION);
    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // 命名空间结束

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface *create() {
  return new DummyModule();
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
