#include "ConfigPusherApplyAgc.h"

#include <unordered_set>
#include <vector>

#include <string>
#include "Logger.h"
#include "ProtoLogUtil.hpp"

namespace ConfigPusher {
namespace {
struct PlannedAgcTask {
  std::string groupName;
  AGCProto::UpsertGroupRequest upsertReq;
  bool start = false;
};

bool buildAgcPlan(const ConfigPusherProto::AgcConfig &config,
                  std::vector<PlannedAgcTask> *outPlan,
                  std::unordered_set<std::string> *outDesiredGroupNames) {
  if (outPlan == nullptr || outDesiredGroupNames == nullptr) {
    LOG_ERROR("AGC 配置编排失败: 输出参数为空");
    return false;
  }
  outPlan->clear();
  outDesiredGroupNames->clear();

  for (const auto &task : config.groups()) {
    if (!task.has_upsert() || !task.upsert().has_config()) {
      LOG_ERROR("AGC 配置任务缺少 upsert/config");
      return false;
    }
    const auto &groupConfig = task.upsert().config();
    if (groupConfig.group_name().empty()) {
      LOG_ERROR("AGC 配置任务缺少 config.group_name");
      return false;
    }
    if (!outDesiredGroupNames->insert(groupConfig.group_name()).second) {
      LOG_ERROR("AGC 配置任务存在重复控制组名: {}", groupConfig.group_name());
      return false;
    }

    PlannedAgcTask planned;
    planned.groupName = groupConfig.group_name();
    planned.upsertReq = task.upsert();
    planned.start = task.start();
    outPlan->push_back(std::move(planned));
  }

  return true;
}

bool reconcileExistingAgcGroups(const std::unordered_set<std::string> &desiredGroupNames,
                                AGCProto::AGCService::StubInterface *stub) {
  AGCProto::Empty listReq;
  AGCProto::ListGroupsResponse listResp;
  grpc::ClientContext listCtx;
  LOG_INFO("发送 AGC 控制组列表请求报文: {}", formatProtoForLog(listReq));
  auto status = stub->ListGroups(&listCtx, listReq, &listResp);
  if (!status.ok()) {
    LOG_ERROR("获取 AGC 现有控制组列表失败: 请求={}, 原因={}",
              formatProtoForLog(listReq),
              status.error_message());
    return false;
  }
  LOG_INFO("收到 AGC 控制组列表响应报文: {}", formatProtoForLog(listResp));

  for (const auto &existing : listResp.groups()) {
    const auto &groupName = existing.config().group_name();
    if (groupName.empty()) {
      LOG_WARNING("AGC 控制组列表返回空控制组名，跳过本次收敛项");
      continue;
    }
    if (!desiredGroupNames.contains(groupName)) {
      AGCProto::DeleteGroupRequest req;
      req.set_group_name(groupName);
      LOG_INFO("AGC 开始删除 jsonc 未声明的旧控制组: 控制组名={}", groupName);
      LOG_INFO("发送 AGC 删除控制组请求报文: {}", formatProtoForLog(req));
      grpc::ClientContext ctx;
      AGCProto::Empty resp;
      status = stub->DeleteGroup(&ctx, req, &resp);
      if (!status.ok()) {
        LOG_ERROR("AGC 删除旧控制组失败: 控制组名={}, 请求={}, 原因={}",
                  groupName,
                  formatProtoForLog(req),
                  status.error_message());
        return false;
      }
      LOG_INFO("收到 AGC 删除控制组响应报文: {}", formatProtoForLog(resp));
      continue;
    }

    if (existing.state() == AGCProto::GROUP_STATE_RUNNING) {
      AGCProto::StopGroupRequest req;
      req.set_group_name(groupName);
      LOG_INFO("AGC 开始停止运行中的现有控制组，以便按 jsonc 覆盖配置: 控制组名={}", groupName);
      LOG_INFO("发送 AGC 停止控制组请求报文: {}", formatProtoForLog(req));
      grpc::ClientContext ctx;
      AGCProto::Empty resp;
      status = stub->StopGroup(&ctx, req, &resp);
      if (!status.ok()) {
        LOG_ERROR("AGC 停止现有控制组失败: 控制组名={}, 请求={}, 原因={}",
                  groupName,
                  formatProtoForLog(req),
                  status.error_message());
        return false;
      }
      LOG_INFO("收到 AGC 停止控制组响应报文: {}", formatProtoForLog(resp));
    }
  }

  return true;
}
}  // namespace

bool applyAgcConfig(const ConfigPusherProto::AgcConfig& config,
                    AGCProto::AGCService::StubInterface* stub) {
  if (stub == nullptr) {
    LOG_ERROR("AGC gRPC stub 为空");
    return false;
  }

  std::vector<PlannedAgcTask> plan;
  std::unordered_set<std::string> desiredGroupNames;
  if (!buildAgcPlan(config, &plan, &desiredGroupNames)) {
    return false;
  }

  bool ok = true;
  LOG_INFO("开始下发 AGC 配置: 控制组任务数={}", config.groups_size());
  if (!reconcileExistingAgcGroups(desiredGroupNames, stub)) {
    return false;
  }

  for (const auto& planned : plan) {
    const auto& groupConfig = planned.upsertReq.config();
    LOG_INFO("处理 AGC 控制组任务: 控制组名={}, 成员数={}, 下发后是否启动控制组功能={}",
             groupConfig.group_name(),
             groupConfig.members_size(),
             planned.start);

    LOG_INFO("发送 AGC 控制组配置请求报文: {}", formatProtoForLog(planned.upsertReq));
    AGCProto::GroupInfo upsertResp;
    grpc::ClientContext upsertCtx;
    auto status = stub->UpsertGroup(&upsertCtx, planned.upsertReq, &upsertResp);
    if (!status.ok()) {
      LOG_ERROR("AGC 控制组配置失败: 控制组名={}, 请求={}, 原因={}",
                groupConfig.group_name(),
                formatProtoForLog(planned.upsertReq),
                status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 AGC 控制组配置响应报文: {}", formatProtoForLog(upsertResp));
    LOG_INFO("AGC 控制组配置成功: 控制组名={}, conn_id={}, 成员数={}",
             upsertResp.config().group_name(),
             upsertResp.conn_id(),
             groupConfig.members_size());

    if (planned.start) {
      LOG_INFO("AGC 配置任务声明 start=true，当前版本仅保留兼容日志，不再额外调用 StartGroup: 控制组名={}",
               groupConfig.group_name());
    }
    LOG_INFO("AGC 配置任务下发完成，后续是否启动控制组功能将由模块依据当前配置自动判定: 控制组名={}",
             groupConfig.group_name());
  }
  return ok;
}

}  // namespace ConfigPusher
