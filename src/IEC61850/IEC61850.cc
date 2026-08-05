#include "IEC61850.h"

#include <boost/dll.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

#include "IEC61850GrpcService.h"
#include "IEC61850CommandExecutorService.h"
#include "IEC61850RawProtocolStack.h"
#include "IEC61850LibInfo.h"
#include "Logger.h"
#include "ModuleManager.pb.h"
#include "mskdsp/IEC61850Limits.hpp"

namespace {
constexpr auto kDataCenterReconcileInterval = std::chrono::seconds(5);

const std::string &GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(IEC61850LibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(IEC61850LibInfo.VERSION_MAJOR);
    version->set_minor(IEC61850LibInfo.VERSION_MINOR);
    version->set_patch(IEC61850LibInfo.VERSION_PATCH);
    version->set_version(IEC61850LibInfo.VERSION);

    // DataCenter是可降级的异步输出，不声明为阻止模块启动的硬依赖。

    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace IEC61850 {
IEC61850::IEC61850() :
  ModuleInterface(),
  iec61850Service_(std::make_shared<IEC61850GrpcServiceImpl>()),
  commandService_(std::make_shared<IEC61850CommandExecutorServiceImpl>()),
  manager_(std::filesystem::path("./conf/config.db"), MakeRawProtocolStack()) {
  initLibInfo(IEC61850LibInfo);
}
IEC61850::~IEC61850() {}
void IEC61850::start(std::stop_token stopToken) {
  LOG_INFO("IEC61850 模块启动");
  const auto loadStatus = manager_.LoadPersistedConfig();
  if (!loadStatus.ok()) {
    LOG_ERROR("IEC61850加载持久化配置失败: {}", loadStatus.error_message());
  } else {
    manager_.ReconcileDataCenter();
    manager_.RestoreConfiguredIeds();
  }
  iec61850Service_->SetManager(&manager_);
  commandService_->SetManager(&manager_);
  LOG_INFO("IEC61850 业务和同步命令服务实例绑定完成");
  std::vector<std::shared_ptr<grpc::Service>> services{
      iec61850Service_, commandService_};
  grpcServerBuilder(services, mskdsp::kIec61850MaxGrpcMessageBytes);
  LOG_INFO("IEC61850 业务和同步命令gRPC服务已启动");

  std::mutex mutex;
  std::condition_variable_any condition;
  std::stop_callback callback(stopToken, [&condition]() { condition.notify_all(); });
  std::unique_lock lock(mutex);
  while (!stopToken.stop_requested()) {
    if (condition.wait_for(
            lock, kDataCenterReconcileInterval,
            [&stopToken]() { return stopToken.stop_requested(); })) {
      break;
    }
    lock.unlock();
    manager_.ReconcileDataCenter();
    lock.lock();
  }
  manager_.Shutdown();
  LOG_INFO("IEC61850 模块停止");
}
}  // namespace IEC61850

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new IEC61850::IEC61850();
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
