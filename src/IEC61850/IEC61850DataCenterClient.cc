#include "IEC61850DataCenterClient.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <utility>

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include "mskdsp/IEC61850Limits.hpp"

namespace IEC61850 {
namespace {

constexpr auto kRpcTimeout = std::chrono::milliseconds(1000);

void SetDeadline(grpc::ClientContext* context) {
  context->set_deadline(std::chrono::system_clock::now() + kRpcTimeout);
}

}  // namespace

DataCenterClient::DataCenterClient(std::string moduleName) :
  moduleName_(std::move(moduleName)), serverAddress_(DefaultServerAddress()) {}

void DataCenterClient::SetStub(
    std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  std::lock_guard lock(mutex_);
  stub_ = std::move(stub);
  channel_.reset();
}

void DataCenterClient::SetServerAddress(std::string address) {
  std::lock_guard lock(mutex_);
  serverAddress_ = std::move(address);
  stub_.reset();
  channel_.reset();
}

grpc::Status DataCenterClient::GetOrCreateConnection(
    const std::string& connName, DataCenterProto::ConnectionInfo* out) {
  if (connName.empty() || out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "conn_name和out不能为空");
  }
  DataCenterProto::GetOrCreateConnectionRequest request;
  request.mutable_key()->set_module_name(moduleName_);
  request.mutable_key()->set_conn_name(connName);
  grpc::ClientContext context;
  SetDeadline(&context);
  out->Clear();
  return GetStub()->GetOrCreateConnection(&context, request, out);
}

grpc::Status DataCenterClient::DeleteConnection(const std::string& connName) {
  if (connName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "conn_name不能为空");
  }
  DataCenterProto::DeleteConnectionRequest request;
  request.mutable_key()->set_module_name(moduleName_);
  request.mutable_key()->set_conn_name(connName);
  DataCenterProto::Empty response;
  grpc::ClientContext context;
  SetDeadline(&context);
  return GetStub()->DeleteConnection(&context, request, &response);
}

grpc::Status DataCenterClient::UpsertConnTags(
    uint32_t connId, const std::vector<std::string>& tags, bool replace) {
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "conn_id不能为空");
  }
  DataCenterProto::UpsertConnTagsRequest request;
  request.set_conn_id(connId);
  request.set_replace(replace);
  for (const auto& tag : tags) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "tags不能包含空字符串");
    }
    request.add_tags(tag);
  }
  DataCenterProto::Empty response;
  grpc::ClientContext context;
  SetDeadline(&context);
  return GetStub()->UpsertConnTags(&context, request, &response);
}

grpc::Status DataCenterClient::BatchPublish(
    const DataCenterProto::BatchPublishRequest& request) {
  auto context = CreateBatchPublishContext();
  return BatchPublish(request, context.get());
}

std::unique_ptr<grpc::ClientContext>
DataCenterClient::CreateBatchPublishContext() const {
  auto context = std::make_unique<grpc::ClientContext>();
  SetDeadline(context.get());
  return context;
}

grpc::Status DataCenterClient::BatchPublish(
    const DataCenterProto::BatchPublishRequest& request,
    grpc::ClientContext* context) {
  if (request.points().empty()) {
    return grpc::Status::OK;
  }
  if (context == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "DataCenter发布context不能为空");
  }
  if (request.ByteSizeLong() >
      mskdsp::kIec61850MaxMmsBatchSerializedBytes) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "DataCenter批量发布请求超过字节安全上限");
  }
  DataCenterProto::Empty response;
  return GetStub()->BatchPublish(context, request, &response);
}

std::shared_ptr<DataCenterProto::DataCenterService::StubInterface>
DataCenterClient::GetStub() {
  std::lock_guard lock(mutex_);
  EnsureStubLocked();
  return stub_;
}

void DataCenterClient::EnsureStubLocked() {
  if (stub_) {
    return;
  }
  channel_ = grpc::CreateChannel(serverAddress_,
                                 grpc::InsecureChannelCredentials());
  auto concrete = DataCenterProto::DataCenterService::NewStub(channel_);
  stub_ = std::shared_ptr<DataCenterProto::DataCenterService::StubInterface>(
      concrete.release());
}

std::string DataCenterClient::DefaultServerAddress() {
  std::error_code error;
  auto directory = std::filesystem::path("./socket");
  std::filesystem::create_directories(directory, error);
  auto absolute = std::filesystem::absolute(directory, error);
  if (error) {
    absolute = directory;
  }
  return std::format("unix:{}", (absolute / "DataCenter.sock").string());
}

}  // namespace IEC61850
