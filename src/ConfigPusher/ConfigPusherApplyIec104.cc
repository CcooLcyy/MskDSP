#include "ConfigPusherApplyIec104.h"

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

bool applyIec104Config(const ConfigPusherProto::Iec104Config &config,
                       IEC104Proto::IEC104Service::StubInterface *stub) {
  if (stub == nullptr) {
    LOG_ERROR("IEC104 gRPC stub 为空");
    return false;
  }

  LOG_INFO("开始下发 IEC104 配置: 任务数={}", config.links_size());
  bool ok = true;
  for (const auto &task : config.links()) {
    if (!task.has_link() || !task.link().has_config()) {
      LOG_ERROR("IEC104 配置任务缺少 link/config");
      ok = false;
      continue;
    }

    const auto &linkConfig = task.link().config();
    if (linkConfig.conn_name().empty()) {
      LOG_ERROR("IEC104 配置任务缺少 config.conn_name");
      ok = false;
      continue;
    }

    LOG_INFO("开始下发 IEC104 连接配置: 连接名={}", linkConfig.conn_name());
    IEC104Proto::UpsertLinkRequest linkReq = task.link();
    LOG_INFO("发送 IEC104 连接配置请求报文: {}", formatProtoForLog(linkReq));
    IEC104Proto::LinkInfo linkInfo;
    grpc::ClientContext linkCtx;
    auto status = stub->UpsertLink(&linkCtx, linkReq, &linkInfo);
    if (!status.ok()) {
      LOG_ERROR("IEC104 连接配置失败: 连接名={}, 请求={}, 原因={}",
                linkConfig.conn_name(),
                formatProtoForLog(linkReq),
                status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 IEC104 连接配置响应报文: {}", formatProtoForLog(linkInfo));
    LOG_INFO("IEC104 连接配置成功: 连接名={}, 连接ID={}", linkConfig.conn_name(), linkInfo.conn_id());

    if (task.has_point_table() && task.point_table().points_size() > 0) {
      IEC104Proto::UpsertPointTableRequest ptReq = task.point_table();
      if (ptReq.conn_name().empty()) {
        ptReq.set_conn_name(linkConfig.conn_name());
      }
      LOG_INFO("开始下发 IEC104 点表: 连接名={}, 点数={}, 是否替换={}",
               ptReq.conn_name(),
               ptReq.points_size(),
               ptReq.replace());
      LOG_INFO("发送 IEC104 点表请求报文: {}", formatProtoForLog(ptReq));
      grpc::ClientContext ptCtx;
      IEC104Proto::Empty ptResp;
      status = stub->UpsertPointTable(&ptCtx, ptReq, &ptResp);
      if (!status.ok()) {
        LOG_ERROR("IEC104 点表下发失败: 连接名={}, 请求={}, 原因={}",
                  ptReq.conn_name(),
                  formatProtoForLog(ptReq),
                  status.error_message());
        ok = false;
        continue;
      }
      LOG_INFO("收到 IEC104 点表响应报文: {}", formatProtoForLog(ptResp));
      LOG_INFO("IEC104 点表下发成功: 连接名={}, 点数={}", ptReq.conn_name(), ptReq.points_size());
    }

    if (task.start()) {
      IEC104Proto::StartLinkRequest startReq;
      startReq.set_conn_name(linkConfig.conn_name());
      LOG_INFO("发送 IEC104 启动连接请求报文: {}", formatProtoForLog(startReq));
      grpc::ClientContext startCtx;
      IEC104Proto::Empty startResp;
      status = stub->StartLink(&startCtx, startReq, &startResp);
      if (!status.ok()) {
        LOG_ERROR("IEC104 启动连接失败: 连接名={}, 请求={}, 原因={}",
                  linkConfig.conn_name(),
                  formatProtoForLog(startReq),
                  status.error_message());
        ok = false;
        continue;
      }
      LOG_INFO("收到 IEC104 启动连接响应报文: {}", formatProtoForLog(startResp));
      LOG_INFO("IEC104 连接启动成功: 连接名={}", linkConfig.conn_name());
    }
  }

  return ok;
}
}  // namespace ConfigPusher
