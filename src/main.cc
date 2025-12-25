#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>

#include "ModuleManager.h"

int main() {
  std::shared_ptr<std::stop_source> stopSource{std::make_shared<std::stop_source>()};
  std::shared_ptr<ModuleManager::ModuleManager> moduleManager{std::make_shared<ModuleManager::ModuleManager>(stopSource)};
  std::jthread thread([&]() { moduleManager->start(); });
}