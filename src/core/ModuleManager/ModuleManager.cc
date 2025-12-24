#include "ModuleManager.h"

#include <stop_token>

#include "ModuleInterface.h"

namespace ModuleManager {
ModuleManager::ModuleManager(std::stop_token stopToken) :
  ModuleInterface::ModuleInterface(stopToken) {}
ModuleManager::~ModuleManager() {}
void ModuleManager::loadModule() {}
}  // namespace ModuleManager