#pragma once

#include <memory>
#include <stop_token>

#include "ModuleInterface.h"

namespace AVC {
class AVCGrpcServiceImpl;
class AVC : public ModuleInterface::ModuleInterface {
public:
  explicit AVC();
  ~AVC() override;

  void start(std::stop_token stopToken) override;

private:
  std::shared_ptr<AVCGrpcServiceImpl> avcService_;
};
}  // namespace AVC
