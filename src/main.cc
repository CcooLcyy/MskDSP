#include <memory>
#include <stop_token>

#include "ModuleManager.h"

int main() {
  std::stop_source stopSource;
  auto stopToken = stopSource.get_token();
  std::shared_ptr<ModuleManager::ModuleManager> moduleManager{std::make_shared<ModuleManager::ModuleManager>(stopToken)};
  moduleManager->start();
}