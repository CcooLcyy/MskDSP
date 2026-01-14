#include "IEC104.h"

#include <boost/dll.hpp>
#include <condition_variable>
#include <mutex>

#include "IEC104GrpcService.h"
#include "IEC104LibInfo.h"

namespace IEC104 {
IEC104::IEC104() :
  ModuleInterface(),
  iec104Service_(std::make_shared<IEC104GrpcServiceImpl>()),
  linkManager_(IEC104LibInfo.LIB_NAME) {
  initLibInfo(IEC104LibInfo);
}
IEC104::~IEC104() {}
void IEC104::start(std::stop_token stopToken) {
  iec104Service_->getIEC104(this);
  grpcServerBuilder(iec104Service_);

  std::mutex mu;
  std::condition_variable_any cv;
  std::stop_callback cb(stopToken, [&cv]() { cv.notify_all(); });
  std::unique_lock lock(mu);
  cv.wait(lock, [&stopToken]() { return stopToken.stop_requested(); });
}

LinkManager& IEC104::linkManager() {
  return linkManager_;
}

const LinkManager& IEC104::linkManager() const {
  return linkManager_;
}
}  // namespace IEC104

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new IEC104::IEC104();
}
