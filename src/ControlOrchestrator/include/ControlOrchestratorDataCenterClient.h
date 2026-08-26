#pragma once

#include <grpcpp/channel.h>
#include <grpcpp/support/status.h>

#include <memory>
#include <mutex>
#include <string>

#include "DataCenter.grpc.pb.h"

namespace ControlOrchestrator {

class DataCenterClient {
public:
  explicit DataCenterClient(std::string moduleName = "ControlOrchestrator");

  void setStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);
  void setServerAddress(std::string address);
  grpc::Status Execute(const DataCenterProto::ExecuteCommandRequest &request,
                       DataCenterProto::ExecuteCommandResponse *response);

private:
  std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> getStub();
  void ensureStubLocked();
  static std::string buildUnixSocketAddress(const std::string &moduleName);

  std::string moduleName_;
  std::string serverAddress_;
  mutable std::mutex mu_;
  std::shared_ptr<grpc::Channel> channel_;
  std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub_;
};

}  // namespace ControlOrchestrator
