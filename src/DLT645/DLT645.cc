#include "DLT645.h"

#include <boost/dll.hpp>
#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>

#include "DLT645GrpcService.h"
#include "DLT645LibInfo.h"

namespace DLT645 {
DLT645::DLT645() :
  ModuleInterface(),
  dlt645Service_(std::make_shared<DLT645GrpcServiceImpl>()) {
  initLibInfo(DLT645LibInfo);
}
DLT645::~DLT645() {}
void DLT645::start(std::stop_token stopToken) {
  dlt645Service_->getDLT645(this);
  grpcServerBuilder(dlt645Service_);
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
}  // namespace DLT645

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new DLT645::DLT645();
}
