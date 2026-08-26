#include "ControlOrchestratorDataCenterClient.h"

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <filesystem>
#include <chrono>
#include <utility>

namespace ControlOrchestrator {

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
                                       DataCenterProto::ExecuteCommandResponse *response) {
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
  if (request.timeout_ms() > 0) {
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(request.timeout_ms()));
  }
  response->Clear();
  return stub->ExecuteCommand(&context, request, response);
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
