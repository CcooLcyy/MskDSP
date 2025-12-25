#pragma once

#include "ModuleInterface.h"

namespace DataCenter {
class DataCenter : public ModuleInterface::ModuleInterface {
public:
  explicit DataCenter(std::stop_source stopSource);
  virtual ~DataCenter();
  virtual void start() override;
};
}  // namespace DataCenter