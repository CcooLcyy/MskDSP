#pragma once

#include <memory>
#include <stop_token>

#include "DLT645LinkManager.h"
#include "ModuleInterface.h"

namespace DLT645 {
class DLT645GrpcServiceImpl;
class DLT645 : public ModuleInterface::ModuleInterface {
public:
  explicit DLT645();
  ~DLT645() override;

  void start(std::stop_token stopToken) override;
  LinkManager& linkManager();
  const LinkManager& linkManager() const;

private:
  std::shared_ptr<DLT645GrpcServiceImpl> dlt645Service_;
  LinkManager linkManager_;
};
}  // namespace DLT645
