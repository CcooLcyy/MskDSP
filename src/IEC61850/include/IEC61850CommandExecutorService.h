#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "DataCenter.grpc.pb.h"

namespace IEC61850 {

class Manager;

// DataCenter同步命令的gRPC适配层；协议和点映射逻辑由Manager负责。
class IEC61850CommandExecutorServiceImpl
    : public DataCenterProto::CommandExecutor::Service {
public:
  void SetManager(Manager* manager) noexcept;

  grpc::Status ExecuteCommand(
      grpc::ServerContext* context,
      const DataCenterProto::ExecuteCommandRequest* request,
      DataCenterProto::ExecuteCommandResponse* response) override;

private:
  Manager* manager_{nullptr};
};

}  // namespace IEC61850
