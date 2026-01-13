#include "DataCenter.h"

#include <boost/dll.hpp>
#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>

#include "DataCenterGrpcService.h"
#include "dataCenterLibInfo.h"

namespace DataCenter {
DataCenter::DataCenter() :
  ModuleInterface(),
  dataCenterService_(std::make_shared<DataCenterGrpcServiceImpl>()) {
  initLibInfo(dataCenterLibInfo);
}
DataCenter::~DataCenter() {}
void DataCenter::start(std::stop_token stopToken) {
  grpcServerBuilder(dataCenterService_);
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
}  // namespace DataCenter

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new DataCenter::DataCenter();
}
