#include <boost/dll.hpp>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>

#include "ModuleInterface.h"

namespace {
// A minimal module used by unit tests. It intentionally does not start any
// gRPC server to keep tests deterministic and fast.
class DummyModule final : public ModuleInterface::ModuleInterface {
public:
  DummyModule() {
    // Keep the metadata consistent with the produced shared library name so
    // ModuleManager can load/unload it by lib_name.
    static const LibInfo kDummyLibInfo{
        .VERSION_MAJOR = "0",
        .VERSION_MINOR = "0",
        .VERSION_PATCH = "1",
        .VERSION = "0.0.1",
        .LIB_NAME = "Dummy",
    };
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
}  // namespace

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface *create() {
  return new DummyModule();
}

