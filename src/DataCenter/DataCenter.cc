#include "DataCenter.h"

#include <boost/dll.hpp>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>

#include "DataCenterGrpcService.h"
#include "DataCenterLibInfo.h"
#include "Logger.h"

namespace DataCenter {
DataCenter::DataCenter() :
  ModuleInterface(),
  dataCenterService_(std::make_shared<DataCenterGrpcServiceImpl>()) {
  initLibInfo(DataCenterLibInfo);
}
DataCenter::~DataCenter() {}
void DataCenter::start(std::stop_token stopToken) {
  LOG_INFO("DataCenter 模块启动");
  grpcServerBuilder(dataCenterService_);

  std::mutex mu;
  std::condition_variable_any cv;
  std::stop_callback cb(stopToken, [&cv]() { cv.notify_all(); });

  std::unique_lock lock(mu);
  cv.wait(lock, [&stopToken]() { return stopToken.stop_requested(); });
  LOG_INFO("DataCenter 模块停止");
}
}  // namespace DataCenter

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new DataCenter::DataCenter();
}
