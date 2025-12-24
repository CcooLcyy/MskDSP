#pragma once

#include <stop_token>

#include "ModuleInterface.h"

class IEC104 : public ModuleInterface {
public:
  explicit IEC104(std::stop_token stopToken);
  ~IEC104();
  virtual MetaData metaData() override;
  virtual void start() override;
};