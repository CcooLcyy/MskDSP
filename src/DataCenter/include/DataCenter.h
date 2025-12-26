#pragma once

#include <memory>
#include <stop_token>

#include "ModuleInterface.h"

namespace DataCenter {
class DataCenterGrpcServiceImpl;
class DataCenter : public ModuleInterface::ModuleInterface {
public:
  explicit DataCenter();
  virtual ~DataCenter();

  virtual void start(std::stop_token stopToken) override;

private:
  std::shared_ptr<DataCenterGrpcServiceImpl> dataCenterService_;
};
}  // namespace DataCenter