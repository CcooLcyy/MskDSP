#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include <memory>

#include "DataCenter.grpc.pb.h"
#include "DataCenter.h"
#include "DataCenter.pb.h"

namespace DataCenter {
class DataCenterGrpcServiceImpl : public DataCenterProto::DataCenterService::Service {
public:
  void getDataCenter(DataCenter *dataCenter);
  grpc::Status GetDataModule(grpc::ServerContext *context, const DataCenterProto::Empty *, DataCenterProto::Empty *) override;

private:
  DataCenter *dataCenter_;
};
}  // namespace DataCenter