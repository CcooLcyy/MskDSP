#include "AVC.h"

#include <boost/dll.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

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

    auto depDataCenter = manifest.add_dependencies();
    depDataCenter->set_module_name("DataCenter");
    depDataCenter->set_version_range("=0.0.1");

    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace AVC {
AVC::AVC() :
  ModuleInterface(),
  avcService_(std::make_shared<AVCGrpcServiceImpl>()),
  commandService_(std::make_shared<AVCCommandExecutorServiceImpl>()),
  groupManager_(AVCLibInfo.LIB_NAME) {
  initLibInfo(AVCLibInfo);
}
AVC::~AVC() {}
void AVC::start(std::stop_token stopToken) {
  LOG_INFO("AVC 模块启动");
  LOG_INFO("AVC 依赖模块: DataCenter");
  avcService_->getAVC(this);
  commandService_->getAVC(this);
  std::vector<std::shared_ptr<grpc::Service>> services{avcService_, commandService_};
  grpcServerBuilder(services);
  LOG_INFO("AVC 开始在模块启动阶段恢复本地控制组配置");
  auto restoreStatus = groupManager_.LoadPersistedConfig();
  if (!restoreStatus.ok()) {
    LOG_ERROR("AVC 恢复本地控制组配置存在错误: {}", restoreStatus.error_message());
  }
  LOG_INFO("AVC 开始在模块启动阶段检查已恢复控制组是否满足自动启动条件");
  groupManager_.TryAutoStartReadyGroups("模块启动后恢复检查");
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  LOG_INFO("AVC 模块停止");
}

GroupManager &AVC::groupManager() {
  return groupManager_;
}

const GroupManager &AVC::groupManager() const {
  return groupManager_;
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
