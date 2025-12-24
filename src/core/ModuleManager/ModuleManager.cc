#include "ModuleManager.h"

#include <format>
#include <stop_token>

#include "ModuleInterface.h"
#include "moduleManagerLibInfo.h"

namespace ModuleManager {
ModuleManager::ModuleManager(std::stop_token stopToken) :
  ModuleInterface::ModuleInterface(stopToken) {
  metaData_.name = moduleManagerLibInfo::LIB_NAME;
  metaData_.libName = std::format("{}{}{}", "lib", moduleManagerLibInfo::LIB_NAME, ".so");
  ::ModuleInterface::Version versionInfo{
      moduleManagerLibInfo::VERSION_MAJOR,
      moduleManagerLibInfo::VERSION_MINOR,
      moduleManagerLibInfo::VERSION_PATCH,
      moduleManagerLibInfo::VERSION};
  metaData_.version = versionInfo;
  // metaData_.grpcServer
}
ModuleManager::~ModuleManager() {}
::ModuleInterface::MetaData ModuleManager::metaData() {
  return metaData_;
}
void ModuleManager::start() {}
void ModuleManager::loadModule() {}
}  // namespace ModuleManager