#pragma once

#include <grpcpp/channel.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include <chrono>
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
                       DataCenterProto::ExecuteCommandResponse *response,
                       grpc::ServerContext *parentContext = nullptr);
  grpc::Status GetLatest(const DataCenterProto::Endpoint &endpoint,
                         DataCenterProto::GetLatestResponse *response,
                         std::chrono::milliseconds timeout = {},
                         grpc::ServerContext *parentContext = nullptr);
  grpc::Status GetSourceLatest(const DataCenterProto::Endpoint &endpoint,
                               DataCenterProto::GetSourceLatestResponse *response,
                               std::chrono::milliseconds timeout = {},
                               grpc::ServerContext *parentContext = nullptr);

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
