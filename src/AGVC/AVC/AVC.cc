#include "AVC.h"

#include <boost/dll.hpp>
#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>

#include "AVCGrpcService.h"
#include "AVCLibInfo.h"

namespace AVC {
AVC::AVC() :
  ModuleInterface(),
  avcService_(std::make_shared<AVCGrpcServiceImpl>()) {
  initLibInfo(AVCLibInfo);
}
AVC::~AVC() {}
void AVC::start(std::stop_token stopToken) {
  avcService_->getAVC(this);
  grpcServerBuilder(avcService_);
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
}  // namespace AVC

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new AVC::AVC();
}
