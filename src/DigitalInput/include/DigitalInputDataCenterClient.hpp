#pragma once

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "DataCenter.grpc.pb.h"
#include "DataCenter.pb.h"

namespace DigitalInput {

class DigitalInputDataCenterClient {
public:
  DigitalInputDataCenterClient();

  void SetStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);
  void SetServerAddress(std::string address);

  grpc::Status GetOrCreateBoardConnection(DataCenterProto::ConnectionInfo* out);
  grpc::Status RegisterBoardTags(uint32_t connId);
  grpc::Status PublishBool(uint32_t connId, const std::string& tag, bool value, int64_t timestampMs);

private:
  std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> GetStub();
  void EnsureStubLocked();
  static std::string DefaultServerAddress();

  std::string serverAddress_;
  mutable std::mutex mutex_;
  std::shared_ptr<grpc::Channel> channel_;
  std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub_;
};

}  // namespace DigitalInput
