#pragma once

#include <boost/dll.hpp>
#include <memory>
#include <stop_token>
#include <thread>

#include "ModuleInterface.h"
#include "ModuleManager.pb.h"

namespace ModuleManager {
struct LibInfo {
  ModuleInterface::MetaData metaData;
  boost::dll::shared_library lib;
  std::shared_ptr<ModuleInterface::ModuleInterface> instance;
  std::jthread thread;
  std::shared_ptr<std::stop_source> stopSource;
};

class ModuleManager : public ModuleInterface::ModuleInterface {
public:
  explicit ModuleManager(std::shared_ptr<std::stop_source> stopSource);
  ~ModuleManager();

  virtual void start() override;

  void loadModule(ModuleManagerProto::ModuleInfo moduleInfo);
  ModuleManagerProto::ModuleInfos &getModuleInfos();

private:
  void initModuleInfos();
  ModuleManagerProto::ModuleVersion parseVersion(std::string libName);
  ModuleManagerProto::ModuleInfos moduleInfos_;
  std::vector<LibInfo> libInfoVec_;
};
}  // namespace ModuleManager