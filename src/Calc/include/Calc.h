#pragma once

#include <memory>
#include <stop_token>

#include "CalcGroupManager.h"
#include "ModuleInterface.h"

namespace Calc {
class CalcGrpcServiceImpl;

class Calc : public ModuleInterface::ModuleInterface {
public:
  explicit Calc();
  ~Calc() override;

  void start(std::stop_token stopToken) override;

  GroupManager &groupManager();
  const GroupManager &groupManager() const;

private:
  std::shared_ptr<CalcGrpcServiceImpl> calcService_;
  GroupManager groupManager_;
};
}  // namespace Calc
