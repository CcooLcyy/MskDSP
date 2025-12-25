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
};

class ModuleManager : public ModuleInterface::ModuleInterface {
public:
  explicit ModuleManager(std::stop_source stopSource);
  ~ModuleManager();

  virtual ::ModuleInterface::MetaData metaData() override;
  virtual void start() override;

  ModuleManagerProto::ModuleInfos &getModuleInfos();

private:
  void initModuleInfos();
  ModuleManagerProto::ModuleVersion parseVersion(std::string libName);
  ModuleManagerProto::ModuleInfos moduleInfos_;
};
}  // namespace ModuleManager