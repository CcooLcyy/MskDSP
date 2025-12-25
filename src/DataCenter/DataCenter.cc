#include "DataCenter.h"

#include <boost/dll.hpp>
#include <stop_token>

#include "DataCenter.h"
#include "dataCenterLibInfo.h"

namespace DataCenter {
DataCenter::DataCenter(std::stop_source stopSource) :
  ModuleInterface(stopSource) {
  initLibInfo(dataCenterLibInfo);
}
DataCenter::~DataCenter() {}
void DataCenter::start() {}
}  // namespace DataCenter

extern "C" BOOST_SYMBOL_EXPORT DataCenter::DataCenter *create(std::stop_source stopSource) {
  return new DataCenter::DataCenter(stopSource);
}