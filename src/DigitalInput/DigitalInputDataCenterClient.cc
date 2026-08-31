#include "DigitalInputDataCenterClient.hpp"

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <string_view>
#include <utility>

namespace DigitalInput {
namespace {

constexpr const char* kModuleName = "DigitalInput";
constexpr const char* kConnectionName = "board-di";
constexpr std::array<std::string_view, 4> kTags = {"DI1", "DI2", "DI3", "DI4"};
constexpr auto kRpcTimeout = std::chrono::milliseconds(1500);

void SetDeadline(grpc::ClientContext* context) {
  context->set_deadline(std::chrono::system_clock::now() + kRpcTimeout);
}

}  // namespace

DigitalInputDataCenterClient::DigitalInputDataCenterClient() :
  serverAddress_(DefaultServerAddress()) {}

void DigitalInputDataCenterClient::SetStub(
    std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  std::lock_guard lock(mutex_);
  stub_ = std::move(stub);
  channel_.reset();
}

void DigitalInputDataCenterClient::SetServerAddress(std::string address) {
  std::lock_guard lock(mutex_);
  serverAddress_ = std::move(address);
  stub_.reset();
  channel_.reset();
}

grpc::Status DigitalInputDataCenterClient::GetOrCreateBoardConnection(
    DataCenterProto::ConnectionInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "连接信息输出参数为空");
  }
  auto request = DataCenterProto::GetOrCreateConnectionRequest();
  request.mutable_key()->set_module_name(kModuleName);
  request.mutable_key()->set_conn_name(kConnectionName);
  grpc::ClientContext context;
  SetDeadline(&context);
  out->Clear();
  return GetStub()->GetOrCreateConnection(&context, request, out);
}

grpc::Status DigitalInputDataCenterClient::RegisterBoardTags(uint32_t connId) {
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  DataCenterProto::UpsertConnTagsRequest request;
  request.set_conn_id(connId);
  request.set_replace(true);
  for (const auto tag : kTags) {
    request.add_tags(std::string(tag));
  }
  DataCenterProto::Empty response;
  grpc::ClientContext context;
  SetDeadline(&context);
  return GetStub()->UpsertConnTags(&context, request, &response);
}

grpc::Status DigitalInputDataCenterClient::PublishBool(
    uint32_t connId, const std::string& tag, bool value, int64_t timestampMs) {
  if (connId == 0 || tag.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "conn_id 和 tag 不能为空");
  }
  DataCenterProto::PublishRequest request;
  request.set_conn_id(connId);
  request.set_tag(tag);
  request.mutable_value()->set_bool_value(value);
  request.set_quality(DataCenterProto::QUALITY_GOOD);
  if (timestampMs > 0) {
    request.set_ts_ms(timestampMs);
  }
  DataCenterProto::Empty response;
  grpc::ClientContext context;
  SetDeadline(&context);
  return GetStub()->Publish(&context, request, &response);
}

std::shared_ptr<DataCenterProto::DataCenterService::StubInterface>
DigitalInputDataCenterClient::GetStub() {
  std::lock_guard lock(mutex_);
  EnsureStubLocked();
  return stub_;
}

void DigitalInputDataCenterClient::EnsureStubLocked() {
  if (stub_) {
    return;
  }
  channel_ = grpc::CreateChannel(serverAddress_, grpc::InsecureChannelCredentials());
  auto concrete = DataCenterProto::DataCenterService::NewStub(channel_);
  stub_ = std::shared_ptr<DataCenterProto::DataCenterService::StubInterface>(concrete.release());
}

std::string DigitalInputDataCenterClient::DefaultServerAddress() {
  std::error_code error;
  const std::filesystem::path socketDirectory = "./socket";
  std::filesystem::create_directories(socketDirectory, error);
  auto absoluteDirectory = std::filesystem::canonical(socketDirectory, error);
  if (error) {
    absoluteDirectory = std::filesystem::absolute(socketDirectory, error);
  }
  return std::format("unix:{}/DataCenter.sock", absoluteDirectory.string());
}

}  // namespace DigitalInput
