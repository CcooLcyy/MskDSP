#pragma once

#include <memory>

#include "ModuleInterface.h"

namespace DataCenter {
class DataCenter : public ModuleInterface::ModuleInterface {
public:
  explicit DataCenter(std::shared_ptr<std::stop_source> stopSource);
  virtual ~DataCenter();
  virtual void start() override;
};
}  // namespace DataCenter