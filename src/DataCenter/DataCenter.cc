#include "DataCenter.h"

#include <boost/dll.hpp>
#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>

#include "DataCenter.h"
#include "dataCenterLibInfo.h"

namespace DataCenter {
DataCenter::DataCenter(std::shared_ptr<std::stop_source> stopSource) :
  ModuleInterface(stopSource) {
  initLibInfo(dataCenterLibInfo);
}
DataCenter::~DataCenter() {}
void DataCenter::start() {
  while (!stopToken_.stop_requested()) {
    std::cout << "正在运行DataCenter" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
}  // namespace DataCenter

extern "C" BOOST_SYMBOL_EXPORT DataCenter::DataCenter *create(void *stopSource) {
  std::shared_ptr<std::stop_source> stopSourcePtr = std::shared_ptr<std::stop_source>(reinterpret_cast<std::stop_source *>(stopSource));
  return new DataCenter::DataCenter(stopSourcePtr);
}