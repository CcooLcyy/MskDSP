#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include "DataCenter.grpc.pb.h"
#include "DataCenter.pb.h"

namespace IEC61850 {

class DataCenterClient {
public:
  explicit DataCenterClient(std::string moduleName);

  void SetStub(
      std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);
  void SetServerAddress(std::string address);

  grpc::Status GetOrCreateConnection(
      const std::string& connName, DataCenterProto::ConnectionInfo* out);
  grpc::Status DeleteConnection(const std::string& connName);
  grpc::Status UpsertConnTags(uint32_t connId,
                              const std::vector<std::string>& tags,
                              bool replace);
  grpc::Status BatchPublish(
      const DataCenterProto::BatchPublishRequest& request);
  std::unique_ptr<grpc::ClientContext> CreateBatchPublishContext() const;
  grpc::Status BatchPublish(
      const DataCenterProto::BatchPublishRequest& request,
      grpc::ClientContext* context);

private:
  std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> GetStub();
  void EnsureStubLocked();
  static std::string DefaultServerAddress();

  std::string moduleName_;
  std::string serverAddress_;
  std::mutex mutex_;
  std::shared_ptr<grpc::Channel> channel_;
  std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub_;
};

}  // namespace IEC61850
