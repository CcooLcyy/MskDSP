#include "AGC.h"

#include <boost/dll.hpp>
#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>

#include "AGCGrpcService.h"
#include "AGCLibInfo.h"

namespace AGC {
AGC::AGC() :
  ModuleInterface(),
  agcService_(std::make_shared<AGCGrpcServiceImpl>()),
  groupManager_(AGCLibInfo.LIB_NAME) {
  initLibInfo(AGCLibInfo);
}
AGC::~AGC() {}
void AGC::start(std::stop_token stopToken) {
  agcService_->getAGC(this);
  grpcServerBuilder(agcService_);
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

GroupManager& AGC::groupManager() {
  return groupManager_;
}

const GroupManager& AGC::groupManager() const {
  return groupManager_;
}
}  // namespace AGC

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new AGC::AGC();
}
