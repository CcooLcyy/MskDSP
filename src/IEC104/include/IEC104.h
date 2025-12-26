#pragma once

#include <memory>
#include <stop_token>

#include "ModuleInterface.h"

namespace IEC104 {
class IEC104GrpcServiceImpl;
class IEC104 : public ModuleInterface::ModuleInterface {
public:
  explicit IEC104();
  virtual ~IEC104();
  virtual void start(std::stop_token stopToken) override;

private:
  std::shared_ptr<IEC104GrpcServiceImpl> iec104Service_;
};
}  // namespace IEC104