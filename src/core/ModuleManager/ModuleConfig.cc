#include "ModuleConfig.h"

#include <google/protobuf/util/json_util.h>

#include <fstream>
#include <sstream>

#include "ModuleConfig.h"
#include "ModuleConfig.pb.h"

ModuleConfig::ModuleConfig() {
}
ModuleConfig::~ModuleConfig() {}
ModuleConfigProto::Config &ModuleConfig::getConfig() {
  return configMessage_;
}
void ModuleConfig::loadConfig() {
  auto ifs{std::ifstream(defaultConfigPath_)};
  std::ostringstream oss;
  if (ifs.is_open()) {
    oss << ifs.rdbuf();
  }

  ModuleConfigProto::Config libInfo;
  auto status = google::protobuf::util::JsonStringToMessage(oss.str(), &libInfo);
}