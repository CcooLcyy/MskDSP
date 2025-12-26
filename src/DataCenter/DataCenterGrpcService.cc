#include "DataCenterGrpcService.h"

#include <grpcpp/support/status.h>

#include <memory>

#include "DataCenter.h"
#include "DataCenterGrpcService.h"

namespace DataCenter {
void DataCenterGrpcServiceImpl::getDataCenter(DataCenter *dataCenter) {
  dataCenter_ = dataCenter;
}
grpc::Status DataCenterGrpcServiceImpl::GetDataModule(grpc::ServerContext *context, const DataCenterProto::Empty *, DataCenterProto::Empty *) {
  return grpc::Status::OK;
}
}  // namespace DataCenter