#include "IEC104.h"

#include <boost/dll.hpp>

#include "IEC104GrpcService.h"
#include "IEC104LibInfo.h"

namespace IEC104 {
IEC104::IEC104() :
  ModuleInterface(),
  iec104Service_(std::make_shared<IEC104GrpcServiceImpl>()) {
  initLibInfo(IEC104LibInfo);
}
IEC104::~IEC104() {}
void IEC104::start(std::stop_token stopToken) {
  iec104Service_->getIEC104(this);
  grpcServerBuilder(iec104Service_);
  while (!stopToken.stop_requested()) {
    std::cout << "正在运行DataCenter" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  std::cout << "DataCenter模块停止" << std::endl;
}
}  // namespace IEC104

extern "C" BOOST_SYMBOL_EXPORT IEC104::IEC104 *create() {
  return new IEC104::IEC104();
}