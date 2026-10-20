#include <memory>
#include <stop_token>
#include <thread>

#include "Logger.h"
#include "ModuleManager.h"
#include "PlatformAccessSupervisor.h"
#include "ThreadUtil.hpp"

int main() {
  std::shared_ptr<std::stop_source> stopSource{std::make_shared<std::stop_source>()};
  std::shared_ptr<ModuleManager::ModuleManager> moduleManager{std::make_shared<ModuleManager::ModuleManager>()};
  std::jthread thread([&]() {
    // 控制器仅加载持久配置；损坏配置关闭接入，不影响现场业务模块。
    auto platformAccess = std::make_shared<mskdsp::platformagent::PlatformAccessSupervisor>("./conf/platform_access.json");
    moduleManager->SetPlatformAccessControl(platformAccess);
    auto platformThread = ModuleManager::StartModuleThread("PlatformAgent", [platformAccess](std::stop_token stop) {
      platformAccess->Run(stop);
    });
    moduleManager->start(stopSource->get_token());
  });
}
