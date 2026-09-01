#include "ControlOrchestratorDataCenterClient.h"

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <stop_token>
#include <thread>
#include <utility>

namespace ControlOrchestrator {
namespace {

class ParentCancellationWatcher {
public:
  ParentCancellationWatcher(grpc::ServerContext *parent, grpc::ClientContext *child) :
    parent_(parent),
    child_(child) {
    if (parent_ == nullptr) {
      return;
    }
    watcher_ = std::jthread([this](std::stop_token stopToken) {
      while (!stopToken.stop_requested()) {
        if (parent_->IsCancelled()) {
          cancelled_.store(true, std::memory_order_release);
          child_->TryCancel();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }

  ~ParentCancellationWatcher() {
    watcher_.request_stop();
    if (watcher_.joinable()) {
      watcher_.join();
    }
  }

  bool cancelled() const { return cancelled_.load(std::memory_order_acquire); }

private:
  grpc::ServerContext *parent_;
  grpc::ClientContext *child_;
  std::atomic_bool cancelled_{false};
  std::jthread watcher_;
};

void setDeadline(grpc::ClientContext *context, std::chrono::milliseconds timeout,
                 grpc::ServerContext *parent) {
  if (context == nullptr) {
    return;
  }
  auto deadline = std::chrono::system_clock::time_point::max();
  const auto now = std::chrono::system_clock::now();
  if (timeout.count() > 0) {
    deadline = now + timeout;
  }
  if (parent != nullptr && parent->deadline() < deadline) {
    deadline = parent->deadline();
  }
  if (deadline != std::chrono::system_clock::time_point::max()) {
    context->set_deadline(deadline);
  }
}

}  // namespace

DataCenterClient::DataCenterClient(std::string moduleName) :
  moduleName_(std::move(moduleName)),
  serverAddress_(buildUnixSocketAddress("DataCenter")) {}

void DataCenterClient::setStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  std::lock_guard<std::mutex> lock(mu_);
  stub_ = std::move(stub);
  channel_.reset();
}

void DataCenterClient::setServerAddress(std::string address) {
  std::lock_guard<std::mutex> lock(mu_);
  serverAddress_ = std::move(address);
  stub_.reset();
  channel_.reset();
}

grpc::Status DataCenterClient::Execute(const DataCenterProto::ExecuteCommandRequest &request,
                                       DataCenterProto::ExecuteCommandResponse *response,
                                       grpc::ServerContext *parentContext) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  if (!request.has_src() || request.src().tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "命令源端点或标签为空");
  }
  if (request.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "命令值为空");
  }
  auto stub = getStub();
  if (!stub) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "DataCenter 服务不可用");
  }
  grpc::ClientContext context;
  setDeadline(&context, std::chrono::milliseconds(request.timeout_ms()), parentContext);
  ParentCancellationWatcher cancellationWatcher(parentContext, &context);
  response->Clear();
  auto status = stub->ExecuteCommand(&context, request, response);
  if (cancellationWatcher.cancelled() ||
      (parentContext != nullptr && parentContext->IsCancelled())) {
    return grpc::Status(grpc::StatusCode::CANCELLED, "上游编排请求已取消");
  }
  return status;
}

grpc::Status DataCenterClient::GetLatest(const DataCenterProto::Endpoint &endpoint,
                                         DataCenterProto::GetLatestResponse *response,
                                         std::chrono::milliseconds timeout,
                                         grpc::ServerContext *parentContext) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  if (endpoint.tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "状态端点必须包含 tag");
  }
  auto stub = getStub();
  if (!stub) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "DataCenter 服务不可用");
  }
  response->Clear();
  DataCenterProto::GetLatestRequest request;
  uint32_t connId = endpoint.conn_id();
  if (connId == 0 && !endpoint.module_name().empty() && !endpoint.conn_name().empty()) {
    DataCenterProto::ListConnectionsResponse connections;
    DataCenterProto::Empty empty;
    grpc::ClientContext resolveContext;
    setDeadline(&resolveContext, timeout, parentContext);
    ParentCancellationWatcher cancellationWatcher(parentContext, &resolveContext);
    auto status = stub->ListConnections(&resolveContext, empty, &connections);
    if (cancellationWatcher.cancelled() ||
        (parentContext != nullptr && parentContext->IsCancelled())) {
      return grpc::Status(grpc::StatusCode::CANCELLED, "上游编排请求已取消");
    }
    if (!status.ok()) return status;
    for (const auto &connection : connections.conns()) {
      if (connection.module_name() == endpoint.module_name() &&
          connection.conn_name() == endpoint.conn_name()) {
        connId = connection.conn_id();
        break;
      }
    }
  }
  grpc::ClientContext context;
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "状态端点连接未在 DataCenter 注册");
  }
  request.set_conn_id(connId);
  request.add_tags(endpoint.tag());
  setDeadline(&context, timeout, parentContext);
  ParentCancellationWatcher cancellationWatcher(parentContext, &context);
  auto status = stub->GetLatest(&context, request, response);
  if (cancellationWatcher.cancelled() ||
      (parentContext != nullptr && parentContext->IsCancelled())) {
    return grpc::Status(grpc::StatusCode::CANCELLED, "上游编排请求已取消");
  }
  return status;
}

grpc::Status DataCenterClient::GetSourceLatest(
    const DataCenterProto::Endpoint &endpoint,
    DataCenterProto::GetSourceLatestResponse *response,
    std::chrono::milliseconds timeout,
    grpc::ServerContext *parentContext) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  if (endpoint.tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "状态端点必须包含 tag");
  }
  auto stub = getStub();
  if (!stub) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "DataCenter 服务不可用");
  }
  response->Clear();
  DataCenterProto::GetSourceLatestRequest request;
  uint32_t connId = endpoint.conn_id();
  if (connId == 0 && !endpoint.module_name().empty() && !endpoint.conn_name().empty()) {
    DataCenterProto::ListConnectionsResponse connections;
    DataCenterProto::Empty empty;
    grpc::ClientContext resolveContext;
    setDeadline(&resolveContext, timeout, parentContext);
    ParentCancellationWatcher cancellationWatcher(parentContext, &resolveContext);
    auto status = stub->ListConnections(&resolveContext, empty, &connections);
    if (cancellationWatcher.cancelled() ||
        (parentContext != nullptr && parentContext->IsCancelled())) {
      return grpc::Status(grpc::StatusCode::CANCELLED, "上游编排请求已取消");
    }
    if (!status.ok()) return status;
    for (const auto &connection : connections.conns()) {
      if (connection.module_name() == endpoint.module_name() &&
          connection.conn_name() == endpoint.conn_name()) {
        connId = connection.conn_id();
        break;
      }
    }
  }
  grpc::ClientContext context;
  if (connId == 0) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "状态端点连接未在 DataCenter 注册");
  }
  request.set_conn_id(connId);
  request.add_tags(endpoint.tag());
  setDeadline(&context, timeout, parentContext);
  ParentCancellationWatcher cancellationWatcher(parentContext, &context);
  auto status = stub->GetSourceLatest(&context, request, response);
  if (cancellationWatcher.cancelled() ||
      (parentContext != nullptr && parentContext->IsCancelled())) {
    return grpc::Status(grpc::StatusCode::CANCELLED, "上游编排请求已取消");
  }
  return status;
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
  channel_ = grpc::CreateChannel(serverAddress_, grpc::InsecureChannelCredentials());
  stub_ = std::shared_ptr<DataCenterProto::DataCenterService::StubInterface>(
      DataCenterProto::DataCenterService::NewStub(channel_).release());
}

std::string DataCenterClient::buildUnixSocketAddress(const std::string &moduleName) {
  std::error_code ec;
  auto dir = std::filesystem::absolute("./socket", ec);
  if (ec) {
    dir = "./socket";
  }
  return "unix:" + (dir / (moduleName + ".sock")).string();
}

}  // namespace ControlOrchestrator
