#pragma once

#include <memory>
#include <stop_token>

#include "ModuleInterface.h"
#include "AVCGroupManager.h"

namespace AVC {
class AVCGrpcServiceImpl;
class AVCCommandExecutorServiceImpl;
class AVC : public ModuleInterface::ModuleInterface {
public:
  explicit AVC();
  ~AVC() override;

  void start(std::stop_token stopToken) override;

  GroupManager& groupManager();
  const GroupManager& groupManager() const;

private:
  std::shared_ptr<AVCGrpcServiceImpl> avcService_;
  std::shared_ptr<AVCCommandExecutorServiceImpl> commandService_;
  GroupManager groupManager_;
};
}  // namespace AVC
