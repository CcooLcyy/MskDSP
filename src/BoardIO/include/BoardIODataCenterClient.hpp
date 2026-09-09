#pragma once

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "DataCenter.grpc.pb.h"
#include "DataCenter.pb.h"

namespace BoardIO {

class BoardIODataCenterClient {
public:
  BoardIODataCenterClient();

  void SetStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);
  void SetServerAddress(std::string address);

  grpc::Status GetOrMigrateInputConnection(DataCenterProto::ConnectionInfo* out);
  grpc::Status GetOrCreateOutputConnection(DataCenterProto::ConnectionInfo* out);
  grpc::Status RegisterInputTags(uint32_t connId);
  grpc::Status RegisterOutputTags(uint32_t connId);
  grpc::Status EnsureInputTags(uint32_t connId);
  grpc::Status EnsureOutputTags(uint32_t connId);
  grpc::Status PublishBool(uint32_t connId, const std::string& tag, bool value, int64_t timestampMs);

private:
  grpc::Status GetOrCreateConnection(const std::string& connectionName,
                                     DataCenterProto::ConnectionInfo* out);
  grpc::Status RegisterTags(uint32_t connId,
                            const std::vector<std::string_view>& tags);
  grpc::Status EnsureTags(uint32_t connId,
                          const std::vector<std::string_view>& tags);
  std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> GetStub();
  void EnsureStubLocked();
  static std::string DefaultServerAddress();

  std::string serverAddress_;
  mutable std::mutex mutex_;
  std::shared_ptr<grpc::Channel> channel_;
  std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub_;
  std::atomic_bool inputMigrationCompleted_{false};
};

}  // namespace BoardIO
