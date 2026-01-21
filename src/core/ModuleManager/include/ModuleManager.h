#pragma once

#include <boost/dll.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Logger.h"
#include "ModuleInterface.h"
#include "ModuleManager.pb.h"

namespace ModuleManager {
class ModuleManagerServiceImpl;

enum class ModuleOpError {
  kOk,
  kNotFound,
  kInvalidArgument,
  kFailedPrecondition,
  kInternal,
};

struct ModuleOpResult {
  ModuleOpError error;
  std::string message;

  bool ok() const {
    return error == ModuleOpError::kOk;
  }
};

class LibInfo {
public:
  LibInfo() = default;
  LibInfo(const LibInfo &) = delete;
  LibInfo &operator=(const LibInfo &) = delete;

  static std::shared_ptr<LibInfo> create(ModuleManagerProto::ModuleInfo moduleInfo) {
    auto module = std::shared_ptr<LibInfo>(new LibInfo());

    module->stopSource = {std::make_shared<std::stop_source>()};
    auto libPath = std::string("./module/") + moduleInfo.lib_name();
    LOG_INFO("加载模块库: {}", libPath);
    module->lib.load(libPath);
    auto create = module->lib.get<ModuleInterface::ModuleInterface *()>("create");
    module->instance = std::shared_ptr<ModuleInterface::ModuleInterface>((create()));
    module->metaData = module->instance->metaData();
    module->thread = std::jthread([module]() {
      LogModuleScope moduleScope(module->metaData.name);
      module->instance->start(module->stopSource->get_token());
    });
    return module;
  }
  ModuleInterface::MetaData MetaData() {
    return metaData;
  }
  void cleanUp() {
    if (stopSource) {
      stopSource->request_stop();
    }
    if (instance) {
      instance->shutdownServers();
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

  ModuleOpResult loadModule(ModuleManagerProto::ModuleInfo moduleInfo);
  ModuleOpResult unloadModule(ModuleManagerProto::ModuleInfo moduleInfo);
  ModuleManagerProto::ModuleInfos &getModuleInfos();
  ModuleManagerProto::ModuleRunningInfos getModuleRunningInfos();
  void saveModuleStartConfig(ModuleManagerProto::ModuleInfos moduleInfos);

private:
  void autoStartModulesFromConfig();
  void ensureModuleInfos();
  void initModuleInfos();
  ModuleManagerProto::ModuleVersion parseVersion(std::string libName);
  ModuleOpResult resolveModuleName(const ModuleManagerProto::ModuleInfo &moduleInfo, std::string *moduleName);
  ModuleOpResult startModuleByName(const std::string &moduleName);
  ModuleOpResult stopModuleByName(const std::string &moduleName);
  ModuleOpResult resolveStartOrder(const std::string &moduleName, std::vector<std::string> *order);
  ModuleOpResult resolveStopOrder(const std::string &moduleName, std::vector<std::string> *order);
  bool isModuleRunning(const std::string &moduleName) const;
  bool stopRunningModuleByName(const std::string &moduleName);

  ModuleManagerProto::ModuleInfos moduleInfos_;
  std::vector<std::shared_ptr<LibInfo>> libInfoVec_;
  std::shared_ptr<ModuleManagerServiceImpl> moduleManagerService_;
  ModuleManagerProto::ModuleInfos moduleConfig_;
  std::unordered_map<std::string, ModuleManagerProto::ModuleInfo> moduleInfoByName_;
  std::unordered_map<std::string, std::vector<std::string>> reverseDependencies_;
  bool moduleInfosReady_{false};
};
}  // namespace ModuleManager
