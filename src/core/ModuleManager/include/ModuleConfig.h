#pragma once

#include <google/protobuf/message.h>

#include <boost/json.hpp>

#include "ModuleConfig.pb.h"

class ModuleConfig {
public:
  ModuleConfig();
  ~ModuleConfig();

  ModuleConfigProto::Config &getConfig();

private:
  const std::string defaultConfigPath_{"./conf/moduleConfig.json"};
  void loadConfig();
  ModuleConfigProto::Config configMessage_;
};