#include "ConfigPusherApplyComMock.h"

#include <google/protobuf/message.h>

#include <string>

#include "Logger.h"

namespace ConfigPusher {
namespace {
std::string formatProtoForLog(const google::protobuf::Message &message) {
  auto text = message.ShortDebugString();
  if (text.empty()) {
    return "空";
  }
  return text;
}
}  // namespace

bool applyComMockConfig(const COMMockProto::COMMockConfig &config,
                        COMMockProto::COMMockService::StubInterface *stub) {
  if (stub == nullptr) {
    LOG_ERROR("COMMock gRPC stub 为空");
    return false;
  }

  LOG_INFO("发送 COMMock 配置下发请求报文: {}", formatProtoForLog(config));
  grpc::ClientContext ctx;
  COMMockProto::Empty resp;
  auto status = stub->ApplyConfig(&ctx, config, &resp);
  if (!status.ok()) {
    LOG_ERROR("COMMock 配置下发失败: 请求={}, 原因={}", formatProtoForLog(config), status.error_message());
    return false;
  }
  LOG_INFO("收到 COMMock 配置下发响应报文: {}", formatProtoForLog(resp));
  LOG_INFO("COMMock 配置下发成功: 端口数={}", config.ports_size());
  return true;
}
}  // namespace ConfigPusher
