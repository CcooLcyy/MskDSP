#pragma once

#include <memory>
#include <stop_token>

#include "ModuleInterface.h"

namespace AGC {
class AGCGrpcServiceImpl;
class AGC : public ModuleInterface::ModuleInterface {
public:
  explicit AGC();
  ~AGC() override;

  void start(std::stop_token stopToken) override;

private:
  std::shared_ptr<AGCGrpcServiceImpl> agcService_;
};
}  // namespace AGC
