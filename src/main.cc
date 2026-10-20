#include <memory>
#include <optional>
#include <stop_token>
#include <thread>

#include "Logger.h"
#include "ModuleManager.h"
#include "PlatformAgent.h"
#include "ThreadUtil.hpp"

int main() {
  std::shared_ptr<std::stop_source> stopSource{std::make_shared<std::stop_source>()};
  std::shared_ptr<ModuleManager::ModuleManager> moduleManager{std::make_shared<ModuleManager::ModuleManager>()};
  std::jthread thread([&]() {
    // 连接功能随管理器线程存活；无效平台配置不能阻止现场模块运行。
    std::optional<mskdsp::platformagent::Config> platformConfig;
    try {
      platformConfig = mskdsp::platformagent::LoadEnvironmentConfig();
    } catch (const std::exception &error) {
      LOG_ERROR("平台连接功能配置无效: {}", error.what());
    }
    std::jthread platformThread;
    if (platformConfig) {
      platformThread = ModuleManager::StartModuleThread("PlatformAgent", [config = *platformConfig](std::stop_token stop) {
        mskdsp::platformagent::PlatformAgent(config).Run(stop);
      });
    }
    moduleManager->start(stopSource->get_token());
  });
}
