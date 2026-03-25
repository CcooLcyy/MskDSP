#include "ConfigPusherApplyModbusRtu.h"

#include <google/protobuf/message.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "Logger.h"
#include "ProtoLogUtil.hpp"

namespace ConfigPusher {
namespace {
struct PlannedModbusTask {
  std::string connName;
  ModbusRTUProto::UpsertLinkRequest linkReq;
  ModbusRTUProto::UpsertPointTableRequest pointTableReq;
  bool start = false;
};

bool buildModbusPlan(const ConfigPusherProto::ModbusRtuConfig &config,
                     std::vector<PlannedModbusTask> *outPlan,
                     std::unordered_set<std::string> *outDesiredConnNames,
                     bool *outNeedsMqttConfig) {
  if (outPlan == nullptr || outDesiredConnNames == nullptr || outNeedsMqttConfig == nullptr) {
    LOG_ERROR("ModbusRTU 配置编排失败: 输出参数为空");
    return false;
  }
  outPlan->clear();
  outDesiredConnNames->clear();
  *outNeedsMqttConfig = false;

  for (const auto &task : config.links()) {
    if (!task.has_link() || !task.link().has_config()) {
      LOG_ERROR("ModbusRTU 配置任务缺少 link/config");
      return false;
    }

    const auto &linkConfig = task.link().config();
    if (linkConfig.conn_name().empty()) {
      LOG_ERROR("ModbusRTU 配置任务缺少 config.conn_name");
      return false;
    }
    if (!outDesiredConnNames->insert(linkConfig.conn_name()).second) {
      LOG_ERROR("ModbusRTU 配置任务存在重复连接名: {}", linkConfig.conn_name());
      return false;
    }
    if (linkConfig.transport_type() == ModbusRTUProto::TRANSPORT_MQTT_UART) {
      *outNeedsMqttConfig = true;
    }

    PlannedModbusTask planned;
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

bool reconcileExistingModbusLinks(const std::unordered_set<std::string> &desiredConnNames,
                                  ModbusRTUProto::ModbusRTUService::StubInterface *stub) {
  ModbusRTUProto::Empty listReq;
  ModbusRTUProto::ListLinksResponse listResp;
  grpc::ClientContext listCtx;
  LOG_INFO("发送 ModbusRTU 链路列表请求报文: {}", formatProtoForLog(listReq));
  auto status = stub->ListLinks(&listCtx, listReq, &listResp);
  if (!status.ok()) {
    LOG_ERROR("获取 ModbusRTU 现有链路列表失败: 请求={}, 原因={}",
              formatProtoForLog(listReq),
              status.error_message());
    return false;
  }
  LOG_INFO("收到 ModbusRTU 链路列表响应报文: {}", formatProtoForLog(listResp));

  for (const auto &existing : listResp.links()) {
    const auto &connName = existing.config().conn_name();
    if (connName.empty()) {
      LOG_WARNING("ModbusRTU 链路列表返回空连接名，跳过本次收敛项");
      continue;
    }
    if (!desiredConnNames.contains(connName)) {
      ModbusRTUProto::DeleteLinkRequest req;
      req.set_conn_name(connName);
      LOG_INFO("ModbusRTU 开始删除 jsonc 未声明的旧链路: 连接名={}", connName);
      LOG_INFO("发送 ModbusRTU 删除链路请求报文: {}", formatProtoForLog(req));
      grpc::ClientContext ctx;
      ModbusRTUProto::Empty resp;
      status = stub->DeleteLink(&ctx, req, &resp);
      if (!status.ok()) {
        LOG_ERROR("ModbusRTU 删除旧链路失败: 连接名={}, 请求={}, 原因={}",
                  connName,
                  formatProtoForLog(req),
                  status.error_message());
        return false;
      }
      LOG_INFO("收到 ModbusRTU 删除链路响应报文: {}", formatProtoForLog(resp));
      continue;
    }

    if (existing.state() == ModbusRTUProto::LINK_STATE_RUNNING) {
      ModbusRTUProto::StopLinkRequest req;
      req.set_conn_name(connName);
      LOG_INFO("ModbusRTU 开始停止运行中的现有链路，以便按 jsonc 覆盖配置: 连接名={}", connName);
      LOG_INFO("发送 ModbusRTU 停止链路请求报文: {}", formatProtoForLog(req));
      grpc::ClientContext ctx;
      ModbusRTUProto::Empty resp;
      status = stub->StopLink(&ctx, req, &resp);
      if (!status.ok()) {
        LOG_ERROR("ModbusRTU 停止现有链路失败: 连接名={}, 请求={}, 原因={}",
                  connName,
                  formatProtoForLog(req),
                  status.error_message());
        return false;
      }
      LOG_INFO("收到 ModbusRTU 停止链路响应报文: {}", formatProtoForLog(resp));
    }
  }

  return true;
}
}  // namespace

bool applyModbusRtuConfig(const ConfigPusherProto::ModbusRtuConfig &config,
                          ModbusRTUProto::ModbusRTUService::StubInterface *stub) {
  if (stub == nullptr) {
    LOG_ERROR("ModbusRTU gRPC stub 为空");
    return false;
  }

  std::vector<PlannedModbusTask> plan;
  std::unordered_set<std::string> desiredConnNames;
  bool needsMqttConfig = false;
  if (!buildModbusPlan(config, &plan, &desiredConnNames, &needsMqttConfig)) {
    return false;
  }

  if (!config.has_mqtt() && needsMqttConfig) {
    LOG_ERROR("ModbusRTU 存在 MQTT 链路，但缺少 mqtt 顶层配置");
    return false;
  }

  if (!reconcileExistingModbusLinks(desiredConnNames, stub)) {
    return false;
  }

  if (config.has_mqtt()) {
    ModbusRTUProto::UpdateConfigRequest req;
    *req.mutable_mqtt() = config.mqtt();
    ModbusRTUProto::UpdateConfigResponse resp;
    LOG_INFO("发送 ModbusRTU MQTT 配置请求报文: {}", formatProtoForLog(req));
    grpc::ClientContext ctx;
    auto status = stub->UpdateConfig(&ctx, req, &resp);
    if (!status.ok() || !resp.ok()) {
      LOG_ERROR("ModbusRTU MQTT 配置下发失败: 请求={}, 原因={}",
                formatProtoForLog(req),
                status.ok() ? resp.message() : status.error_message());
      if (needsMqttConfig) {
        return false;
      }
    } else {
      LOG_INFO("收到 ModbusRTU MQTT 配置响应报文: {}", formatProtoForLog(resp));
      LOG_INFO("ModbusRTU MQTT 配置下发成功");
    }
  }

  LOG_INFO("开始下发 ModbusRTU 配置: 任务数={}", config.links_size());
  bool ok = true;
  for (const auto &planned : plan) {
    LOG_INFO("开始下发 ModbusRTU 连接配置: 连接名={}", planned.connName);
    LOG_INFO("发送 ModbusRTU 连接配置请求报文: {}", formatProtoForLog(planned.linkReq));
    ModbusRTUProto::LinkInfo linkInfo;
    grpc::ClientContext linkCtx;
    auto status = stub->UpsertLink(&linkCtx, planned.linkReq, &linkInfo);
    if (!status.ok()) {
      LOG_ERROR("ModbusRTU 连接配置失败: 连接名={}, 请求={}, 原因={}",
                planned.connName,
                formatProtoForLog(planned.linkReq),
                status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 ModbusRTU 连接配置响应报文: {}", formatProtoForLog(linkInfo));
    LOG_INFO("ModbusRTU 连接配置成功: 连接名={}, 连接ID={}", planned.connName, linkInfo.conn_id());

    LOG_INFO("开始按全量覆盖语义下发 ModbusRTU 点表: 连接名={}, 点数={}, 是否替换={}",
             planned.pointTableReq.conn_name(),
             planned.pointTableReq.points_size(),
             planned.pointTableReq.replace());
    LOG_INFO("发送 ModbusRTU 点表请求报文: {}", formatProtoForLog(planned.pointTableReq));
    grpc::ClientContext ptCtx;
    ModbusRTUProto::Empty ptResp;
    status = stub->UpsertPointTable(&ptCtx, planned.pointTableReq, &ptResp);
    if (!status.ok()) {
      LOG_ERROR("ModbusRTU 点表下发失败: 连接名={}, 请求={}, 原因={}",
                planned.pointTableReq.conn_name(),
                formatProtoForLog(planned.pointTableReq),
                status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 ModbusRTU 点表响应报文: {}", formatProtoForLog(ptResp));
    LOG_INFO("ModbusRTU 点表下发成功: 连接名={}, 点数={}",
             planned.pointTableReq.conn_name(),
             planned.pointTableReq.points_size());

    if (planned.start) {
      LOG_INFO("ModbusRTU 配置任务声明 start=true，当前版本仅保留兼容日志，不再额外调用 StartLink: 连接名={}",
               planned.connName);
    }
    LOG_INFO("ModbusRTU 配置任务下发完成，后续是否启动连接功能将由模块依据当前配置自动判定: 连接名={}",
             planned.connName);
  }

  return ok;
}
}  // namespace ConfigPusher
