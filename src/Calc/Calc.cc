#include "Calc.h"

#include <boost/dll.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>

#include "CalcGrpcService.h"
#include "CalcLibInfo.h"
#include "Logger.h"
#include "ModuleManager.pb.h"

namespace {
const std::string &GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(CalcLibInfo.LIB_NAME);
    auto *version = manifest.mutable_version();
    version->set_major(CalcLibInfo.VERSION_MAJOR);
    version->set_minor(CalcLibInfo.VERSION_MINOR);
    version->set_patch(CalcLibInfo.VERSION_PATCH);
    version->set_version(CalcLibInfo.VERSION);

    auto *depDataCenter = manifest.add_dependencies();
    depDataCenter->set_module_name("DataCenter");
    depDataCenter->set_version_range("=0.0.1");

    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace Calc {

Calc::Calc() :
  ModuleInterface(),
  calcService_(std::make_shared<CalcGrpcServiceImpl>()),
  groupManager_(CalcLibInfo.LIB_NAME) {
  initLibInfo(CalcLibInfo);
}

Calc::~Calc() {}

void Calc::start(std::stop_token stopToken) {
  LOG_INFO("Calc 模块启动");
  LOG_INFO("Calc 依赖模块: DataCenter");
  calcService_->getCalc(this);
  grpcServerBuilder(calcService_);
  LOG_INFO("Calc 开始在模块启动阶段恢复本地分组配置");
  auto restoreStatus = groupManager_.LoadPersistedConfig();
  if (!restoreStatus.ok()) {
    LOG_ERROR("Calc 恢复本地分组配置存在错误: {}", restoreStatus.error_message());
  }
  LOG_INFO("Calc 开始在模块启动阶段检查已恢复分组是否满足自动启动条件");
  groupManager_.TryAutoStartReadyGroups("模块启动后恢复检查");
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  LOG_INFO("Calc 模块停止");
}

GroupManager &Calc::groupManager() {
  return groupManager_;
}

const GroupManager &Calc::groupManager() const {
  return groupManager_;
}

}  // namespace Calc

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface *create() {
  return new Calc::Calc();
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
