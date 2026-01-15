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

// A minimal module used by unit tests. It intentionally does not start any
// gRPC server to keep tests deterministic and fast.
class DummyModule final : public ModuleInterface::ModuleInterface {
public:
  DummyModule() {
    // Keep the metadata consistent with the produced shared library name so
    // ModuleManager can load/unload it by lib_name.
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
}  // namespace

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
