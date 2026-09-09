#include "BoardIODataCenterClient.hpp"

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <string_view>
#include <utility>

#include "Logger.h"

namespace BoardIO {
namespace {

constexpr const char* kModuleName = "BoardIO";
constexpr const char* kLegacyModuleName = "DigitalInput";
constexpr const char* kInputConnectionName = "board-di";
constexpr const char* kOutputConnectionName = "board-do";
const std::vector<std::string_view> kInputTags = {"DI1", "DI2", "DI3", "DI4"};
const std::vector<std::string_view> kOutputTags = {"DO1", "DO2"};
constexpr auto kRpcTimeout = std::chrono::milliseconds(1500);

void SetDeadline(grpc::ClientContext* context) {
  context->set_deadline(std::chrono::system_clock::now() + kRpcTimeout);
}

}  // namespace

BoardIODataCenterClient::BoardIODataCenterClient() :
  serverAddress_(DefaultServerAddress()) {}

void BoardIODataCenterClient::SetStub(
    std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  std::lock_guard lock(mutex_);
  stub_ = std::move(stub);
  channel_.reset();
  inputMigrationCompleted_.store(false, std::memory_order_release);
}

void BoardIODataCenterClient::SetServerAddress(std::string address) {
  std::lock_guard lock(mutex_);
  serverAddress_ = std::move(address);
  stub_.reset();
  channel_.reset();
  inputMigrationCompleted_.store(false, std::memory_order_release);
}

grpc::Status BoardIODataCenterClient::GetOrMigrateInputConnection(
    DataCenterProto::ConnectionInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "连接信息输出参数为空");
  }
  if (inputMigrationCompleted_.load(std::memory_order_acquire)) {
    return GetOrCreateConnection(kInputConnectionName, out);
  }

  DataCenterProto::RenameConnectionRequest request;
  request.mutable_old_key()->set_module_name(kLegacyModuleName);
  request.mutable_old_key()->set_conn_name(kInputConnectionName);
  request.mutable_new_key()->set_module_name(kModuleName);
  request.mutable_new_key()->set_conn_name(kInputConnectionName);
  grpc::ClientContext context;
  SetDeadline(&context);
  out->Clear();
  auto status = GetStub()->RenameConnection(&context, request, out);
  if (status.ok()) {
    inputMigrationCompleted_.store(true, std::memory_order_release);
    LOG_INFO("BoardIO 已迁移旧遥信端点: DigitalInput/board-di -> BoardIO/board-di, conn_id={}",
             out->conn_id());
    return status;
  }
  if (status.error_code() == grpc::StatusCode::ALREADY_EXISTS) {
    inputMigrationCompleted_.store(true, std::memory_order_release);
    LOG_WARNING("BoardIO 遥信端点已存在，跳过旧端点迁移并使用现有 BoardIO/board-di");
    return GetOrCreateConnection(kInputConnectionName, out);
  }
  if (status.error_code() != grpc::StatusCode::NOT_FOUND) {
    LOG_ERROR("BoardIO 迁移旧遥信端点失败: {}", status.error_message());
    return status;
  }
  LOG_INFO("BoardIO 未发现旧遥信端点，将创建 BoardIO/board-di");
  status = GetOrCreateConnection(kInputConnectionName, out);
  if (status.ok()) {
    inputMigrationCompleted_.store(true, std::memory_order_release);
  }
  return status;
}

grpc::Status BoardIODataCenterClient::GetOrCreateOutputConnection(
    DataCenterProto::ConnectionInfo* out) {
  return GetOrCreateConnection(kOutputConnectionName, out);
}

grpc::Status BoardIODataCenterClient::GetOrCreateConnection(
    const std::string& connectionName, DataCenterProto::ConnectionInfo* out) {
  if (out == nullptr || connectionName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "连接名称或连接信息输出参数为空");
  }
  auto request = DataCenterProto::GetOrCreateConnectionRequest();
  request.mutable_key()->set_module_name(kModuleName);
  request.mutable_key()->set_conn_name(connectionName);
  grpc::ClientContext context;
  SetDeadline(&context);
  out->Clear();
  return GetStub()->GetOrCreateConnection(&context, request, out);
}

grpc::Status BoardIODataCenterClient::RegisterInputTags(uint32_t connId) {
  return RegisterTags(connId, kInputTags);
}

grpc::Status BoardIODataCenterClient::RegisterOutputTags(uint32_t connId) {
  return RegisterTags(connId, kOutputTags);
}

grpc::Status BoardIODataCenterClient::EnsureInputTags(uint32_t connId) {
  return EnsureTags(connId, kInputTags);
}

grpc::Status BoardIODataCenterClient::EnsureOutputTags(uint32_t connId) {
  return EnsureTags(connId, kOutputTags);
}

grpc::Status BoardIODataCenterClient::RegisterTags(
    uint32_t connId, const std::vector<std::string_view>& tags) {
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  DataCenterProto::UpsertConnTagsRequest request;
  request.set_conn_id(connId);
  request.set_replace(true);
  for (const auto tag : tags) {
    request.add_tags(std::string(tag));
  }
  DataCenterProto::Empty response;
  grpc::ClientContext context;
  SetDeadline(&context);
  return GetStub()->UpsertConnTags(&context, request, &response);
}

grpc::Status BoardIODataCenterClient::EnsureTags(
    uint32_t connId, const std::vector<std::string_view>& tags) {
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "conn_id 不能为空");
  }
  DataCenterProto::GetConnTagsRequest request;
  request.set_conn_id(connId);
  DataCenterProto::ConnTags response;
  grpc::ClientContext context;
  SetDeadline(&context);
  const auto status = GetStub()->GetConnTags(&context, request, &response);
  if (status.ok()) {
    const bool tagsMatch = response.tags_size() ==
                               static_cast<int>(tags.size()) &&
                           std::all_of(tags.begin(), tags.end(),
                                       [&response](std::string_view expected) {
                                         return std::find_if(
                                                    response.tags().begin(),
                                                    response.tags().end(),
                                                    [expected](const std::string& actual) {
                                                      return std::string_view(actual) ==
                                                             expected;
                                                    }) != response.tags().end();
                                       });
    if (tagsMatch) {
      return grpc::Status::OK;
    }
    LOG_WARNING("BoardIO 检测到 DataCenter 连接标签不完整，将重新注册: conn_id={}",
                connId);
    return RegisterTags(connId, tags);
  }
  if (status.error_code() == grpc::StatusCode::NOT_FOUND) {
    LOG_INFO("BoardIO 检测到 DataCenter 连接标签缺失，将重新注册: conn_id={}",
             connId);
    return RegisterTags(connId, tags);
  }
  return status;
}

grpc::Status BoardIODataCenterClient::PublishBool(
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
BoardIODataCenterClient::GetStub() {
  std::lock_guard lock(mutex_);
  EnsureStubLocked();
  return stub_;
}

void BoardIODataCenterClient::EnsureStubLocked() {
  if (stub_) {
    return;
  }
  channel_ = grpc::CreateChannel(serverAddress_, grpc::InsecureChannelCredentials());
  auto concrete = DataCenterProto::DataCenterService::NewStub(channel_);
  stub_ = std::shared_ptr<DataCenterProto::DataCenterService::StubInterface>(concrete.release());
}

std::string BoardIODataCenterClient::DefaultServerAddress() {
  std::error_code error;
  const std::filesystem::path socketDirectory = "./socket";
  std::filesystem::create_directories(socketDirectory, error);
  auto absoluteDirectory = std::filesystem::canonical(socketDirectory, error);
  if (error) {
    absoluteDirectory = std::filesystem::absolute(socketDirectory, error);
  }
  return std::format("unix:{}/DataCenter.sock", absoluteDirectory.string());
}

}  // namespace BoardIO
