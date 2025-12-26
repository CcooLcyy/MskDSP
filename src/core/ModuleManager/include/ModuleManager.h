#pragma once

#include <boost/dll.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <memory>
#include <stop_token>
#include <thread>

#include "ModuleInterface.h"
#include "ModuleManager.pb.h"

namespace ModuleManager {
class ModuleManagerServiceImpl;
class LibInfo {
public:
  LibInfo() = default;
  LibInfo(const LibInfo &) = delete;
  LibInfo &operator=(const LibInfo &) = delete;

  static std::shared_ptr<LibInfo> create(ModuleManagerProto::ModuleInfo moduleInfo) {
    auto module = std::shared_ptr<LibInfo>(new LibInfo());

    module->stopSource = {std::make_shared<std::stop_source>()};
    module->lib.load(std::string("./lib/") + moduleInfo.lib_name());
    auto create = module->lib.get<ModuleInterface::ModuleInterface *()>("create");
    module->instance = std::shared_ptr<ModuleInterface::ModuleInterface>((create()));
    std::jthread([module]() { module->instance->start(module->stopSource->get_token()); }).detach();
    module->metaData = module->instance->metaData();
    return module;
  }
  ModuleInterface::MetaData MetaData() {
    return metaData;
  }
  void cleanUp() {
    if (stopSource) {
      stopSource->request_stop();
    }
    if (thread.joinable()) {
      thread.join();
    }
    instance.reset();
    if (lib.is_loaded()) {
      lib.unload();
    }
    stopSource.reset();
  }

private:
  std::shared_ptr<std::stop_source> stopSource;
  boost::dll::shared_library lib;
  std::shared_ptr<ModuleInterface::ModuleInterface> instance;
  std::jthread thread;
  ModuleInterface::MetaData metaData;
};

class ModuleManager : public ModuleInterface::ModuleInterface {
public:
  explicit ModuleManager();
  ~ModuleManager();

  virtual void start(std::stop_token stopToken) override;

  void loadModule(ModuleManagerProto::ModuleInfo moduleInfo);
  void unloadModule(ModuleManagerProto::ModuleInfo moduleInfo);
  ModuleManagerProto::ModuleInfos &getModuleInfos();
  ModuleManagerProto::ModuleRunningInfos getModuleRunningInfos();

private:
  void initModuleInfos();
  ModuleManagerProto::ModuleVersion parseVersion(std::string libName);
  ModuleManagerProto::ModuleInfos moduleInfos_;
  std::vector<std::shared_ptr<LibInfo>> libInfoVec_;
  std::shared_ptr<ModuleManagerServiceImpl> moduleManagerService_;
};
}  // namespace ModuleManager