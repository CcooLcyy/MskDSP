#include "ConfigPusherGrpcService.h"

#include <grpcpp/support/status.h>

#include <string>

#include "Logger.h"

namespace ConfigPusher {
void ConfigPusherGrpcServiceImpl::getConfigPusher(ConfigPusher* module) {
  module_ = module;
}
grpc::Status ConfigPusherGrpcServiceImpl::Ping(grpc::ServerContext* context,
                                               const ConfigPusherProto::Empty *request,
                                               ConfigPusherProto::Empty *response) {
  std::string reqText = "空";
  if (request != nullptr) {
    auto text = request->ShortDebugString();
    if (!text.empty()) {
      reqText = std::move(text);
    }
  }
  LOG_INFO("收到 ConfigPusher Ping 请求报文: {}", reqText);
  std::string respText = "空";
  if (response != nullptr) {
    auto text = response->ShortDebugString();
    if (!text.empty()) {
      respText = std::move(text);
    }
  }
  LOG_INFO("返回 ConfigPusher Ping 响应报文: {}", respText);
  return grpc::Status::OK;
}
}  // namespace ConfigPusher
