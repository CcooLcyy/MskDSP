#pragma once

#include <stop_token>

#include "ModuleInterface.h"

namespace IEC104 {
class IEC104 : public ModuleInterface::ModuleInterface {
public:
  explicit IEC104(std::stop_token stopToken);
  ~IEC104();
  virtual ::ModuleInterface::MetaData metaData() override;
  virtual void start() override;
};
}  // namespace IEC104