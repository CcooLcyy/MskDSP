#include "ConfigPusherApplyIec104.h"

#include <google/protobuf/message.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "Logger.h"
#include "ProtoLogUtil.hpp"

namespace ConfigPusher {
namespace {
struct PlannedIec104Task {
  std::string connName;
  IEC104Proto::UpsertLinkRequest linkReq;
  IEC104Proto::UpsertPointTableRequest pointTableReq;
  bool start = false;
};

bool buildIec104Plan(const ConfigPusherProto::Iec104Config &config,
                     std::vector<PlannedIec104Task> *outPlan,
                     std::unordered_set<std::string> *outDesiredConnNames) {
  if (outPlan == nullptr || outDesiredConnNames == nullptr) {
    LOG_ERROR("IEC104 配置编排失败: 输出参数为空");
    return false;
  }
  outPlan->clear();
  outDesiredConnNames->clear();

  for (const auto &task : config.links()) {
    if (!task.has_link() || !task.link().has_config()) {
      LOG_ERROR("IEC104 配置任务缺少 link/config");
      return false;
    }

    const auto &linkConfig = task.link().config();
    if (linkConfig.conn_name().empty()) {
      LOG_ERROR("IEC104 配置任务缺少 config.conn_name");
      return false;
    }

    if (!outDesiredConnNames->insert(linkConfig.conn_name()).second) {
      LOG_ERROR("IEC104 配置任务存在重复连接名: {}", linkConfig.conn_name());
      return false;
    }

    PlannedIec104Task planned;
    planned.connName = linkConfig.conn_name();
    planned.linkReq = task.link();
    planned.pointTableReq.set_conn_name(linkConfig.conn_name());
    planned.pointTableReq.set_replace(true);
    if (task.has_point_table()) {
      planned.pointTableReq = task.point_table();
      if (planned.pointTableReq.conn_name().empty()) {
        planned.pointTableReq.set_conn_name(linkConfig.conn_name());
      }
      // 在 CONFIG_PUSHER 编排下，jsonc 是最终真相源；点表统一按全量覆盖语义收敛。
      planned.pointTableReq.set_replace(true);
    }
    planned.start = task.start();
    outPlan->push_back(std::move(planned));
  }

  return true;
}

bool reconcileExistingIec104Links(const std::unordered_set<std::string> &desiredConnNames,
                                  IEC104Proto::IEC104Service::StubInterface *stub) {
  IEC104Proto::Empty listReq;
  IEC104Proto::ListLinksResponse listResp;
  grpc::ClientContext listCtx;
  LOG_INFO("发送 IEC104 链路列表请求报文: {}", formatProtoForLog(listReq));
  auto status = stub->ListLinks(&listCtx, listReq, &listResp);
  if (!status.ok()) {
    LOG_ERROR("获取 IEC104 现有链路列表失败: 请求={}, 原因={}",
              formatProtoForLog(listReq),
              status.error_message());
    return false;
  }
  LOG_INFO("收到 IEC104 链路列表响应报文: {}", formatProtoForLog(listResp));

  for (const auto &existing : listResp.links()) {
    const auto &connName = existing.config().conn_name();
    if (connName.empty()) {
      LOG_WARNING("IEC104 链路列表返回空连接名，跳过本次收敛项");
      continue;
    }
    if (!desiredConnNames.contains(connName)) {
      IEC104Proto::DeleteLinkRequest req;
      req.set_conn_name(connName);
      LOG_INFO("IEC104 开始删除 jsonc 未声明的旧链路: 连接名={}", connName);
      LOG_INFO("发送 IEC104 删除链路请求报文: {}", formatProtoForLog(req));
      grpc::ClientContext ctx;
      IEC104Proto::Empty resp;
      status = stub->DeleteLink(&ctx, req, &resp);
      if (!status.ok()) {
        LOG_ERROR("IEC104 删除旧链路失败: 连接名={}, 请求={}, 原因={}",
                  connName,
                  formatProtoForLog(req),
                  status.error_message());
        return false;
      }
      LOG_INFO("收到 IEC104 删除链路响应报文: {}", formatProtoForLog(resp));
      continue;
    }

    if (existing.state() == IEC104Proto::LINK_STATE_RUNNING) {
      IEC104Proto::StopLinkRequest req;
      req.set_conn_name(connName);
      LOG_INFO("IEC104 开始停止运行中的现有链路，以便按 jsonc 覆盖配置: 连接名={}", connName);
      LOG_INFO("发送 IEC104 停止链路请求报文: {}", formatProtoForLog(req));
      grpc::ClientContext ctx;
      IEC104Proto::Empty resp;
      status = stub->StopLink(&ctx, req, &resp);
      if (!status.ok()) {
        LOG_ERROR("IEC104 停止现有链路失败: 连接名={}, 请求={}, 原因={}",
                  connName,
                  formatProtoForLog(req),
                  status.error_message());
        return false;
      }
      LOG_INFO("收到 IEC104 停止链路响应报文: {}", formatProtoForLog(resp));
    }
  }

  return true;
}
}  // namespace

bool applyIec104Config(const ConfigPusherProto::Iec104Config &config,
                       IEC104Proto::IEC104Service::StubInterface *stub) {
  if (stub == nullptr) {
    LOG_ERROR("IEC104 gRPC stub 为空");
    return false;
  }

  std::vector<PlannedIec104Task> plan;
  std::unordered_set<std::string> desiredConnNames;
  if (!buildIec104Plan(config, &plan, &desiredConnNames)) {
    return false;
  }

  LOG_INFO("开始下发 IEC104 配置: 任务数={}", config.links_size());
  if (!reconcileExistingIec104Links(desiredConnNames, stub)) {
    return false;
  }

  bool ok = true;
  for (const auto &planned : plan) {
    LOG_INFO("开始下发 IEC104 连接配置: 连接名={}", planned.connName);
    LOG_INFO("发送 IEC104 连接配置请求报文: {}", formatProtoForLog(planned.linkReq));
    IEC104Proto::LinkInfo linkInfo;
    grpc::ClientContext linkCtx;
    auto status = stub->UpsertLink(&linkCtx, planned.linkReq, &linkInfo);
    if (!status.ok()) {
      LOG_ERROR("IEC104 连接配置失败: 连接名={}, 请求={}, 原因={}",
                planned.connName,
                formatProtoForLog(planned.linkReq),
                status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 IEC104 连接配置响应报文: {}", formatProtoForLog(linkInfo));
    LOG_INFO("IEC104 连接配置成功: 连接名={}, 连接ID={}", planned.connName, linkInfo.conn_id());

    LOG_INFO("开始按全量覆盖语义下发 IEC104 点表: 连接名={}, 点数={}, 是否替换={}",
             planned.pointTableReq.conn_name(),
             planned.pointTableReq.points_size(),
             planned.pointTableReq.replace());
    LOG_INFO("发送 IEC104 点表请求报文: {}", formatProtoForLog(planned.pointTableReq));
    grpc::ClientContext ptCtx;
    IEC104Proto::Empty ptResp;
    status = stub->UpsertPointTable(&ptCtx, planned.pointTableReq, &ptResp);
    if (!status.ok()) {
      LOG_ERROR("IEC104 点表下发失败: 连接名={}, 请求={}, 原因={}",
                planned.pointTableReq.conn_name(),
                formatProtoForLog(planned.pointTableReq),
                status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 IEC104 点表响应报文: {}", formatProtoForLog(ptResp));
    LOG_INFO("IEC104 点表下发成功: 连接名={}, 点数={}",
             planned.pointTableReq.conn_name(),
             planned.pointTableReq.points_size());

    if (planned.start) {
      LOG_INFO("IEC104 配置任务声明 start=true，当前版本仅保留兼容日志，不再额外调用 StartLink: 连接名={}",
               planned.connName);
    }
    LOG_INFO("IEC104 配置任务下发完成，后续是否启动连接功能将由模块依据当前配置自动判定: 连接名={}",
             planned.connName);
  }

  return ok;
}
}  // namespace ConfigPusher
