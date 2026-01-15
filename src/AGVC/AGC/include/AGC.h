#pragma once

#include <memory>
#include <stop_token>

#include "ModuleInterface.h"
#include "AGCGroupManager.h"

namespace AGC {
class AGCGrpcServiceImpl;
class AGC : public ModuleInterface::ModuleInterface {
public:
  explicit AGC();
  ~AGC() override;

  void start(std::stop_token stopToken) override;

  GroupManager& groupManager();
  const GroupManager& groupManager() const;

private:
  std::shared_ptr<AGCGrpcServiceImpl> agcService_;
  GroupManager groupManager_;
};
}  // namespace AGC
