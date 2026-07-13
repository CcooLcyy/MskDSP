#include "IEC104DataCenterClient.h"

#include <filesystem>
#include <format>
#include <utility>

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

namespace IEC104 {
namespace {
constexpr const char* kDefaultDataCenterModuleName = "DataCenter";
}  // namespace

DataCenterClient::DataCenterClient(std::string moduleName) :
  moduleName_(std::move(moduleName)),
  serverAddress_(buildUnixSocketAddress(kDefaultDataCenterModuleName)) {}

void DataCenterClient::setStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  std::lock_guard<std::mutex> lock(mu_);
  stub_ = std::move(stub);
  channel_.reset();
}

void DataCenterClient::setServerAddress(std::string address) {
  std::lock_guard<std::mutex> lock(mu_);
  serverAddress_ = std::move(address);
  channel_.reset();
  stub_.reset();
}

grpc::Status DataCenterClient::ConnectionExists(const std::string& connName, bool* outExists) {
  if (outExists == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "outExists 为空");
  }
  *outExists = false;
  if (connName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }
  auto stub = getStub();

  grpc::ClientContext ctx;
  DataCenterProto::Empty req;
  DataCenterProto::ListConnectionsResponse resp;
  auto status = stub->ListConnections(&ctx, req, &resp);
  if (!status.ok()) {
    return status;
  }

  for (const auto& conn : resp.conns()) {
    if (conn.module_name() == moduleName_ && conn.conn_name() == connName) {
      *outExists = true;
      break;
    }
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterClient::GetOrCreateConnection(const std::string& connName, DataCenterProto::ConnectionInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (connName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }
  auto stub = getStub();

  DataCenterProto::GetOrCreateConnectionRequest req;
  auto* key = req.mutable_key();
  key->set_module_name(moduleName_);
  key->set_conn_name(connName);

  grpc::ClientContext ctx;
  out->Clear();
  return stub->GetOrCreateConnection(&ctx, req, out);
}

grpc::Status DataCenterClient::RenameConnection(const std::string& oldConnName,
                                                const std::string& newConnName,
                                                DataCenterProto::ConnectionInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (oldConnName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "old_conn_name 不能为空");
  }
  if (newConnName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "new_conn_name 不能为空");
  }
  auto stub = getStub();

  DataCenterProto::RenameConnectionRequest req;
  auto* oldKey = req.mutable_old_key();
  oldKey->set_module_name(moduleName_);
  oldKey->set_conn_name(oldConnName);
  auto* newKey = req.mutable_new_key();
  newKey->set_module_name(moduleName_);
  newKey->set_conn_name(newConnName);

  grpc::ClientContext ctx;
  out->Clear();
  return stub->RenameConnection(&ctx, req, out);
}

grpc::Status DataCenterClient::DeleteConnection(const std::string& connName) {
  if (connName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }
  auto stub = getStub();

  DataCenterProto::DeleteConnectionRequest req;
  auto* key = req.mutable_key();
  key->set_module_name(moduleName_);
  key->set_conn_name(connName);

  grpc::ClientContext ctx;
  DataCenterProto::Empty resp;
  return stub->DeleteConnection(&ctx, req, &resp);
}

grpc::Status DataCenterClient::UpsertConnTags(uint32_t connId, const std::vector<std::string>& tags, bool replace) {
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  for (const auto& tag : tags) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags 包含空字符串");
    }
  }
  auto stub = getStub();

  DataCenterProto::UpsertConnTagsRequest req;
  req.set_conn_id(connId);
  req.set_replace(replace);
  for (const auto& tag : tags) {
    req.add_tags(tag);
  }

  grpc::ClientContext ctx;
  DataCenterProto::Empty resp;
  return stub->UpsertConnTags(&ctx, req, &resp);
}

grpc::Status DataCenterClient::PublishBool(
    uint32_t connId, const std::string& tag, bool value, DataCenterProto::Quality quality, int64_t tsMs) {
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  if (tag.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag 不能为空");
  }
  auto stub = getStub();

  DataCenterProto::PublishRequest req;
  req.set_conn_id(connId);
  req.set_tag(tag);
  req.mutable_value()->set_bool_value(value);
  if (tsMs > 0) {
    req.set_ts_ms(tsMs);
  }
  req.set_quality(quality);

  grpc::ClientContext ctx;
  DataCenterProto::Empty resp;
  return stub->Publish(&ctx, req, &resp);
}

grpc::Status DataCenterClient::PublishDouble(
    uint32_t connId, const std::string& tag, double value, DataCenterProto::Quality quality, int64_t tsMs) {
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  if (tag.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag 不能为空");
  }
  auto stub = getStub();

  DataCenterProto::PublishRequest req;
  req.set_conn_id(connId);
  req.set_tag(tag);
  req.mutable_value()->set_double_value(value);
  if (tsMs > 0) {
    req.set_ts_ms(tsMs);
  }
  req.set_quality(quality);

  grpc::ClientContext ctx;
  DataCenterProto::Empty resp;
  return stub->Publish(&ctx, req, &resp);
}

grpc::Status DataCenterClient::PublishInt64(
    uint32_t connId, const std::string& tag, int64_t value, DataCenterProto::Quality quality, int64_t tsMs) {
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  if (tag.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag 不能为空");
  }
  auto stub = getStub();

  DataCenterProto::PublishRequest req;
  req.set_conn_id(connId);
  req.set_tag(tag);
  req.mutable_value()->set_int_value(value);
  if (tsMs > 0) {
    req.set_ts_ms(tsMs);
  }
  req.set_quality(quality);

  grpc::ClientContext ctx;
  DataCenterProto::Empty resp;
  return stub->Publish(&ctx, req, &resp);
}

grpc::Status DataCenterClient::ExecuteCommand(
    const DataCenterProto::ExecuteCommandRequest& request,
    DataCenterProto::ExecuteCommandResponse* response) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response 为空");
  }
  if (!request.has_src()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "src 不能为空");
  }
  if (request.src().conn_id() == 0 && (request.src().module_name().empty() || request.src().conn_name().empty())) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "src 连接不能为空");
  }
  if (request.src().tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "src.tag 不能为空");
  }
  if (request.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "value 不能为空");
  }
  auto stub = getStub();

  grpc::ClientContext ctx;
  response->Clear();
  return stub->ExecuteCommand(&ctx, request, response);
}

grpc::Status DataCenterClient::GetLatest(
    uint32_t connId, const std::vector<std::string>& tags, DataCenterProto::GetLatestResponse* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  for (const auto& tag : tags) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags 包含空字符串");
    }
  }
  auto stub = getStub();

  DataCenterProto::GetLatestRequest req;
  req.set_conn_id(connId);
  for (const auto& tag : tags) {
    req.add_tags(tag);
  }

  grpc::ClientContext ctx;
  out->Clear();
  return stub->GetLatest(&ctx, req, out);
}

std::unique_ptr<grpc::ClientReaderInterface<DataCenterProto::PointUpdate>> DataCenterClient::Subscribe(
    grpc::ClientContext* context, uint32_t connId, const std::vector<std::string>& tags, bool snapshot) {
  if (context == nullptr) {
    return nullptr;
  }
  if (connId == 0) {
    return nullptr;
  }
  for (const auto& tag : tags) {
    if (tag.empty()) {
      return nullptr;
    }
  }
  auto stub = getStub();

  DataCenterProto::SubscribeRequest req;
  req.set_conn_id(connId);
  req.set_snapshot(snapshot);
  for (const auto& tag : tags) {
    req.add_tags(tag);
  }

  return stub->Subscribe(context, req);
}

std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> DataCenterClient::getStub() {
  std::lock_guard<std::mutex> lock(mu_);
  ensureStubLocked();
  return stub_;
}

void DataCenterClient::ensureStubLocked() {
  if (stub_) {
    return;
  }
  if (serverAddress_.empty()) {
    serverAddress_ = buildUnixSocketAddress(kDefaultDataCenterModuleName);
  }
  channel_ = grpc::CreateChannel(serverAddress_, grpc::InsecureChannelCredentials());
  auto concrete = DataCenterProto::DataCenterService::NewStub(channel_);
  stub_ = std::shared_ptr<DataCenterProto::DataCenterService::StubInterface>(concrete.release());
}

std::string DataCenterClient::buildUnixSocketAddress(const std::string& moduleName) {
  std::error_code ec;
  auto dir = std::filesystem::path("./socket");
  if (!std::filesystem::exists(dir, ec)) {
    std::filesystem::create_directories(dir, ec);
  }

  auto absDir = std::filesystem::canonical(dir, ec);
  if (ec) {
    absDir = std::filesystem::absolute(dir, ec);
  }
  auto sockPath = absDir / (moduleName + ".sock");
  return std::format("unix:{}", sockPath.string());
}

}  // namespace IEC104
