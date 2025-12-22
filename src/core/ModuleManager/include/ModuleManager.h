#pragma once

#include <boost/dll.hpp>
#include <memory>
#include <thread>

#include "ModuleInterface.h"

struct LibInfo {
  MetaData metaData;
  boost::dll::shared_library lib;
  std::shared_ptr<ModuleInterface> instance;
  std::jthread thread;
};

class ModuleManager {
public:
  ModuleManager(std::string configFile);
  ~ModuleManager();

  void loadModule();
};