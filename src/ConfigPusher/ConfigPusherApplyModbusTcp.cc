#include "ConfigPusherApplyModbusTcp.h"

#include <google/protobuf/message.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "Logger.h"
#include "ProtoLogUtil.hpp"

namespace ConfigPusher {
namespace {
struct PlannedTask {
  std::string connName;
  ModbusTCPProto::UpsertLinkRequest linkReq;
  ModbusTCPProto::UpsertPointTableRequest pointTableReq;
};

bool buildPlan(const ConfigPusherProto::ModbusTcpConfig &config,
               std::vector<PlannedTask> *plan,
               std::unordered_set<std::string> *desiredNames) {
  if (plan == nullptr || desiredNames == nullptr) {
    LOG_ERROR("ModbusTCP 配置编排失败: 输出参数为空");
    return false;
  }
  plan->clear();
  desiredNames->clear();
  for (const auto &task : config.links()) {
    if (!task.has_link() || !task.link().has_config() || task.link().config().conn_name().empty()) {
      LOG_ERROR("ModbusTCP 配置任务缺少 link/config/conn_name");
      return false;
    }
    const auto &name = task.link().config().conn_name();
    if (task.has_point_table() && !task.point_table().conn_name().empty() && task.point_table().conn_name() != name) {
      LOG_ERROR("ModbusTCP 点表连接名与链路不一致: 链路={}, 点表={}", name, task.point_table().conn_name());
      return false;
    }
    if (!desiredNames->insert(name).second) {
      LOG_ERROR("ModbusTCP 配置任务存在重复连接名: {}", name);
      return false;
    }
    PlannedTask planned;
    planned.connName = name;
    planned.linkReq = task.link();
    planned.pointTableReq.set_conn_name(name);
    planned.pointTableReq.set_replace(true);
    if (task.has_point_table()) {
      planned.pointTableReq = task.point_table();
      if (planned.pointTableReq.conn_name().empty()) planned.pointTableReq.set_conn_name(name);
      planned.pointTableReq.set_replace(true);
    }
    plan->push_back(std::move(planned));
  }
  return true;
}

bool reconcile(const std::unordered_set<std::string> &desired,
               ModbusTCPProto::ModbusTCPService::StubInterface *stub) {
  ModbusTCPProto::Empty req;
  ModbusTCPProto::ListLinksResponse resp;
  grpc::ClientContext listCtx;
  LOG_INFO("发送 ModbusTCP 链路列表请求报文: {}", formatProtoForLog(req));
  auto status = stub->ListLinks(&listCtx, req, &resp);
  if (!status.ok()) {
    LOG_ERROR("获取 ModbusTCP 现有链路列表失败: 请求={}, 原因={}", formatProtoForLog(req), status.error_message());
    return false;
  }
  LOG_INFO("收到 ModbusTCP 链路列表响应报文: {}", formatProtoForLog(resp));
  for (const auto &existing : resp.links()) {
    const auto &name = existing.config().conn_name();
    if (name.empty()) {
      LOG_WARNING("ModbusTCP 链路列表返回空连接名，跳过本次收敛项");
      continue;
    }
    if (desired.contains(name)) {
      if (existing.state() != ModbusTCPProto::LINK_STATE_RUNNING) continue;
      ModbusTCPProto::StopLinkRequest stopReq;
      stopReq.set_conn_name(name);
      ModbusTCPProto::Empty stopResp;
      grpc::ClientContext stopCtx;
      LOG_INFO("发送 ModbusTCP 停止链路请求报文: {}", formatProtoForLog(stopReq));
      status = stub->StopLink(&stopCtx, stopReq, &stopResp);
      if (!status.ok()) {
        LOG_ERROR("ModbusTCP 停止现有链路失败: 连接名={}, 原因={}", name, status.error_message());
        return false;
      }
      continue;
    }
    ModbusTCPProto::DeleteLinkRequest deleteReq;
    deleteReq.set_conn_name(name);
    ModbusTCPProto::Empty deleteResp;
    grpc::ClientContext deleteCtx;
    LOG_INFO("发送 ModbusTCP 删除旧链路请求报文: {}", formatProtoForLog(deleteReq));
    status = stub->DeleteLink(&deleteCtx, deleteReq, &deleteResp);
    if (!status.ok()) {
      LOG_ERROR("ModbusTCP 删除 jsonc 未声明的旧链路失败: 连接名={}, 原因={}", name, status.error_message());
      return false;
    }
  }
  return true;
}
}  // namespace

bool applyModbusTcpConfig(const ConfigPusherProto::ModbusTcpConfig &config,
                          ModbusTCPProto::ModbusTCPService::StubInterface *stub) {
  if (stub == nullptr) {
    LOG_ERROR("ModbusTCP gRPC stub 为空");
    return false;
  }
  std::vector<PlannedTask> plan;
  std::unordered_set<std::string> desired;
  if (!buildPlan(config, &plan, &desired) || !reconcile(desired, stub)) return false;

  bool ok = true;
  for (const auto &planned : plan) {
    ModbusTCPProto::LinkInfo info;
    grpc::ClientContext linkCtx;
    LOG_INFO("发送 ModbusTCP 链路配置请求报文: {}", formatProtoForLog(planned.linkReq));
    auto status = stub->UpsertLink(&linkCtx, planned.linkReq, &info);
    if (!status.ok()) {
      LOG_ERROR("ModbusTCP 链路配置失败: 连接名={}, 原因={}", planned.connName, status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 ModbusTCP 链路配置响应报文: {}", formatProtoForLog(info));

    ModbusTCPProto::Empty pointResp;
    grpc::ClientContext pointCtx;
    LOG_INFO("发送 ModbusTCP 点表请求报文: {}", formatProtoForLog(planned.pointTableReq));
    status = stub->UpsertPointTable(&pointCtx, planned.pointTableReq, &pointResp);
    if (!status.ok()) {
      LOG_ERROR("ModbusTCP 点表下发失败: 连接名={}, 原因={}", planned.connName, status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 ModbusTCP 点表响应报文: {}", formatProtoForLog(pointResp));
  }
  LOG_INFO("ModbusTCP 配置下发完成: 任务数={}, 成功={}", config.links_size(), ok);
  return ok;
}
}  // namespace ConfigPusher
