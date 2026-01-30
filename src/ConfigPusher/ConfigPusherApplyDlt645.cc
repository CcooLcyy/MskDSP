#include "ConfigPusherApplyDlt645.h"

#include <google/protobuf/message.h>

#include <string>

#include "Logger.h"

namespace ConfigPusher {
namespace {
std::string formatProtoForLog(const google::protobuf::Message& message) {
  auto text = message.ShortDebugString();
  if (text.empty()) {
    return "空";
  }
  return text;
}
}  // namespace

bool applyDlt645Config(const ConfigPusherProto::Dlt645Config& config,
                       DLT645Proto::DLT645Service::StubInterface* stub) {
  if (stub == nullptr) {
    LOG_ERROR("DLT645 gRPC stub 为空");
    return false;
  }

  bool ok = true;
  if (config.has_mqtt()) {
    DLT645Proto::UpdateConfigRequest req;
    *req.mutable_mqtt() = config.mqtt();
    DLT645Proto::UpdateConfigResponse resp;
    LOG_INFO("发送 DLT645 MQTT 配置请求报文: {}", formatProtoForLog(req));
    grpc::ClientContext ctx;
    auto status = stub->UpdateConfig(&ctx, req, &resp);
    if (!status.ok() || !resp.ok()) {
      LOG_ERROR("DLT645 MQTT 配置下发失败: 请求={}, 原因={}",
                formatProtoForLog(req),
                status.ok() ? resp.message() : status.error_message());
      ok = false;
    } else {
      LOG_INFO("收到 DLT645 MQTT 配置响应报文: {}", formatProtoForLog(resp));
      LOG_INFO("DLT645 MQTT 配置下发成功");
    }
  } else {
    LOG_WARNING("DLT645 配置缺少 MQTT 连接参数");
  }

  LOG_INFO("开始下发 DLT645 配置: 任务数={}", config.links_size());
  for (const auto& task : config.links()) {
    if (!task.has_link() || !task.link().has_config()) {
      LOG_ERROR("DLT645 配置任务缺少 link/config");
      ok = false;
      continue;
    }
    const auto& linkConfig = task.link().config();
    if (linkConfig.conn_name().empty()) {
      LOG_ERROR("DLT645 配置任务缺少 config.conn_name");
      ok = false;
      continue;
    }

    DLT645Proto::UpsertLinkRequest linkReq = task.link();
    LOG_INFO("发送 DLT645 连接配置请求报文: {}", formatProtoForLog(linkReq));
    DLT645Proto::LinkInfo linkInfo;
    grpc::ClientContext linkCtx;
    auto status = stub->UpsertLink(&linkCtx, linkReq, &linkInfo);
    if (!status.ok()) {
      LOG_ERROR("DLT645 连接配置失败: 连接名={}, 请求={}, 原因={}",
                linkConfig.conn_name(),
                formatProtoForLog(linkReq),
                status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 DLT645 连接配置响应报文: {}", formatProtoForLog(linkInfo));
    LOG_INFO("DLT645 连接配置成功: 连接名={}, 连接ID={}", linkConfig.conn_name(), linkInfo.conn_id());

    if (task.has_point_table() && task.point_table().points_size() > 0) {
      DLT645Proto::UpsertPointTableRequest ptReq = task.point_table();
      if (ptReq.conn_name().empty()) {
        ptReq.set_conn_name(linkConfig.conn_name());
      }
      LOG_INFO("发送 DLT645 点表请求报文: {}", formatProtoForLog(ptReq));
      DLT645Proto::Empty ptResp;
      grpc::ClientContext ptCtx;
      status = stub->UpsertPointTable(&ptCtx, ptReq, &ptResp);
      if (!status.ok()) {
        LOG_ERROR("DLT645 点表下发失败: 连接名={}, 请求={}, 原因={}",
                  ptReq.conn_name(),
                  formatProtoForLog(ptReq),
                  status.error_message());
        ok = false;
        continue;
      }
      LOG_INFO("收到 DLT645 点表响应报文: {}", formatProtoForLog(ptResp));
      LOG_INFO("DLT645 点表下发成功: 连接名={}, 点数={}", ptReq.conn_name(), ptReq.points_size());
    }

    if (task.start()) {
      DLT645Proto::StartLinkRequest startReq;
      startReq.set_conn_name(linkConfig.conn_name());
      LOG_INFO("发送 DLT645 启动连接请求报文: {}", formatProtoForLog(startReq));
      DLT645Proto::Empty startResp;
      grpc::ClientContext startCtx;
      status = stub->StartLink(&startCtx, startReq, &startResp);
      if (!status.ok()) {
        LOG_ERROR("DLT645 启动连接失败: 连接名={}, 请求={}, 原因={}",
                  linkConfig.conn_name(),
                  formatProtoForLog(startReq),
                  status.error_message());
        ok = false;
        continue;
      }
      LOG_INFO("收到 DLT645 启动连接响应报文: {}", formatProtoForLog(startResp));
      LOG_INFO("DLT645 连接启动成功: 连接名={}", linkConfig.conn_name());
    }
  }
  return ok;
}
}  // namespace ConfigPusher
