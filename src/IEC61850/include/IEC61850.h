#pragma once

#include <memory>
#include <stop_token>

#include "ModuleInterface.h"
#include "IEC61850Manager.h"

namespace IEC61850 {
class IEC61850GrpcServiceImpl;
class IEC61850CommandExecutorServiceImpl;
class IEC61850 : public ModuleInterface::ModuleInterface {
public:
  explicit IEC61850();
  ~IEC61850() override;

  void start(std::stop_token stopToken) override;

private:
  std::shared_ptr<IEC61850GrpcServiceImpl> iec61850Service_;
  std::shared_ptr<IEC61850CommandExecutorServiceImpl> commandService_;
  Manager manager_;
};
}  // namespace IEC61850
