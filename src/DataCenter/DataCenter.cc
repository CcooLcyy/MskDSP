#include "DataCenter.h"

#include <boost/dll.hpp>
#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>

#include "DataCenter.h"
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
  dataCenterService_->getDataCenter(this);
  grpcServerBuilder(dataCenterService_);
  while (!stopToken.stop_requested()) {
    std::cout << "正在运行DataCenter" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  std::cout << "DataCenter模块停止" << std::endl;
}
}  // namespace DataCenter

extern "C" BOOST_SYMBOL_EXPORT DataCenter::DataCenter *create() {
  return new DataCenter::DataCenter();
}