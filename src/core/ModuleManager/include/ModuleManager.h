#pragma once

#include <boost/dll.hpp>
#include <memory>
#include <thread>

#include "ModuleInterface.h"

namespace ModuleManager {
struct LibInfo {
  ModuleInterface::MetaData metaData;
  boost::dll::shared_library lib;
  std::shared_ptr<ModuleInterface::ModuleInterface> instance;
  std::jthread thread;
};

class ModuleManager : public ModuleInterface::ModuleInterface {
public:
  explicit ModuleManager(std::stop_token stopToken);
  ~ModuleManager();

  virtual ::ModuleInterface::MetaData metaData() override;
  virtual void start() override;

  void loadModule();
};
}  // namespace ModuleManager