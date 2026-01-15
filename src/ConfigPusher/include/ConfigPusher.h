#pragma once

#include <memory>
#include <stop_token>

#include "ModuleInterface.h"

namespace ConfigPusher {
class ConfigPusherGrpcServiceImpl;
class ConfigPusher : public ModuleInterface::ModuleInterface {
public:
  explicit ConfigPusher();
  ~ConfigPusher() override;

  void start(std::stop_token stopToken) override;

private:
  void applyConfig();

  std::shared_ptr<ConfigPusherGrpcServiceImpl> configPusherService_;
};
}  // namespace ConfigPusher
