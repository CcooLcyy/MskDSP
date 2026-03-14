#pragma once

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "DataCenter.grpc.pb.h"
#include "DataCenter.pb.h"

namespace ModbusRTU {

class DataCenterClient {
public:
  explicit DataCenterClient(std::string moduleName);

  void setStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);

  grpc::Status ConnectionExists(const std::string& connName, bool* outExists);
  grpc::Status GetOrCreateConnection(const std::string& connName, DataCenterProto::ConnectionInfo* out);
  grpc::Status DeleteConnection(const std::string& connName);
  grpc::Status UpsertConnTags(uint32_t connId, const std::vector<std::string>& tags, bool replace);
  grpc::Status GetLatest(uint32_t connId, const std::vector<std::string>& tags, DataCenterProto::GetLatestResponse* out);

  grpc::Status PublishBool(uint32_t connId, const std::string& tag, bool value, DataCenterProto::Quality quality, int64_t tsMs);
  grpc::Status PublishUInt16(uint32_t connId, const std::string& tag, uint16_t value, DataCenterProto::Quality quality, int64_t tsMs);
  grpc::Status PublishDouble(uint32_t connId, const std::string& tag, double value, DataCenterProto::Quality quality, int64_t tsMs);
  std::unique_ptr<grpc::ClientReaderInterface<DataCenterProto::PointUpdate>> Subscribe(
      grpc::ClientContext* context, uint32_t connId, const std::vector<std::string>& tags, bool snapshot);

  void setServerAddress(std::string address);

private:
  std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> getStub();
  void ensureStubLocked();
  static std::string buildUnixSocketAddress(const std::string& moduleName);

  std::string moduleName_;
  std::string serverAddress_;
  mutable std::mutex mu_;
  std::shared_ptr<grpc::Channel> channel_;
  std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub_;
};

}  // namespace ModbusRTU
