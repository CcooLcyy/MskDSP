#include "ConfigPusherApplyCalc.h"

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Logger.h"
#include "ProtoLogUtil.hpp"

namespace ConfigPusher {
namespace {
struct PlannedCalcTask {
  std::string groupName;
  CalcProto::UpsertGroupRequest upsertReq;
  bool start = false;
};

bool buildCalcPlan(const ConfigPusherProto::CalcConfig &config,
                   std::vector<PlannedCalcTask> *outPlan,
                   std::unordered_set<std::string> *outDesiredGroupNames) {
  if (outPlan == nullptr || outDesiredGroupNames == nullptr) {
    LOG_ERROR("Calc 配置编排失败: 输出参数为空");
    return false;
  }
  outPlan->clear();
  outDesiredGroupNames->clear();

  for (const auto &task : config.groups()) {
    if (!task.has_upsert() || !task.upsert().has_config()) {
      LOG_ERROR("Calc 配置任务缺少 upsert/config");
      return false;
    }
    const auto &groupConfig = task.upsert().config();
    if (groupConfig.group_name().empty()) {
      LOG_ERROR("Calc 配置任务缺少 config.group_name");
      return false;
    }
    if (!outDesiredGroupNames->insert(groupConfig.group_name()).second) {
      LOG_ERROR("Calc 配置任务存在重复计算分组名: {}", groupConfig.group_name());
      return false;
    }

    PlannedCalcTask planned;
    planned.groupName = groupConfig.group_name();
    planned.upsertReq = task.upsert();
    planned.start = task.start();
    outPlan->push_back(std::move(planned));
  }

  return true;
}

bool reconcileExistingCalcGroups(const std::unordered_set<std::string> &desiredGroupNames,
                                 CalcProto::CalcService::StubInterface *stub) {
  CalcProto::Empty listReq;
  CalcProto::ListGroupsResponse listResp;
  grpc::ClientContext listCtx;
  LOG_INFO("发送 Calc 计算分组列表请求报文: {}", formatProtoForLog(listReq));
  auto status = stub->ListGroups(&listCtx, listReq, &listResp);
  if (!status.ok()) {
    LOG_ERROR("获取 Calc 现有计算分组列表失败: 请求={}, 原因={}",
              formatProtoForLog(listReq),
              status.error_message());
    return false;
  }
  LOG_INFO("收到 Calc 计算分组列表响应报文: {}", formatProtoForLog(listResp));

  for (const auto &existing : listResp.groups()) {
    const auto &groupName = existing.config().group_name();
    if (groupName.empty()) {
      LOG_WARNING("Calc 计算分组列表返回空分组名，跳过本次收敛项");
      continue;
    }
    if (!desiredGroupNames.contains(groupName)) {
      CalcProto::DeleteGroupRequest req;
      req.set_group_name(groupName);
      LOG_INFO("Calc 开始删除 jsonc 未声明的旧计算分组: 分组名={}", groupName);
      LOG_INFO("发送 Calc 删除计算分组请求报文: {}", formatProtoForLog(req));
      grpc::ClientContext ctx;
      CalcProto::Empty resp;
      status = stub->DeleteGroup(&ctx, req, &resp);
      if (!status.ok()) {
        LOG_ERROR("Calc 删除旧计算分组失败: 分组名={}, 请求={}, 原因={}",
                  groupName,
                  formatProtoForLog(req),
                  status.error_message());
        return false;
      }
      LOG_INFO("收到 Calc 删除计算分组响应报文: {}", formatProtoForLog(resp));
      continue;
    }

    if (existing.state() == CalcProto::GROUP_STATE_RUNNING) {
      CalcProto::StopGroupRequest req;
      req.set_group_name(groupName);
      LOG_INFO("Calc 开始停止运行中的现有计算分组，以便按 jsonc 覆盖配置: 分组名={}", groupName);
      LOG_INFO("发送 Calc 停止计算分组请求报文: {}", formatProtoForLog(req));
      grpc::ClientContext ctx;
      CalcProto::Empty resp;
      status = stub->StopGroup(&ctx, req, &resp);
      if (!status.ok()) {
        LOG_ERROR("Calc 停止现有计算分组失败: 分组名={}, 请求={}, 原因={}",
                  groupName,
                  formatProtoForLog(req),
                  status.error_message());
        return false;
      }
      LOG_INFO("收到 Calc 停止计算分组响应报文: {}", formatProtoForLog(resp));
    }
  }

  return true;
}
}  // namespace

bool applyCalcConfig(const ConfigPusherProto::CalcConfig &config,
                     CalcProto::CalcService::StubInterface *stub) {
  if (stub == nullptr) {
    LOG_ERROR("Calc gRPC stub 为空");
    return false;
  }

  std::vector<PlannedCalcTask> plan;
  std::unordered_set<std::string> desiredGroupNames;
  if (!buildCalcPlan(config, &plan, &desiredGroupNames)) {
    return false;
  }

  bool ok = true;
  LOG_INFO("开始下发 Calc 配置: 计算分组任务数={}", config.groups_size());
  if (!reconcileExistingCalcGroups(desiredGroupNames, stub)) {
    return false;
  }

  for (const auto &planned : plan) {
    const auto &groupConfig = planned.upsertReq.config();
    LOG_INFO("处理 Calc 计算分组任务: 分组名={}, 计算项数={}, 下发后是否启动分组运算功能={}",
             groupConfig.group_name(),
             groupConfig.items_size(),
             planned.start);

    LOG_INFO("发送 Calc 计算分组配置请求报文: {}", formatProtoForLog(planned.upsertReq));
    CalcProto::CalcGroupInfo upsertResp;
    grpc::ClientContext upsertCtx;
    auto status = stub->UpsertGroup(&upsertCtx, planned.upsertReq, &upsertResp);
    if (!status.ok()) {
      LOG_ERROR("Calc 计算分组配置失败: 分组名={}, 请求={}, 原因={}",
                groupConfig.group_name(),
                formatProtoForLog(planned.upsertReq),
                status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 Calc 计算分组配置响应报文: {}", formatProtoForLog(upsertResp));
    LOG_INFO("Calc 计算分组配置成功: 分组名={}, conn_id={}, 计算项数={}",
             upsertResp.config().group_name(),
             upsertResp.conn_id(),
             groupConfig.items_size());

    if (planned.start) {
      LOG_INFO("Calc 配置任务声明 start=true，当前版本仅保留兼容日志，不再额外调用 StartGroup: 分组名={}",
               groupConfig.group_name());
    }
    LOG_INFO("Calc 配置任务下发完成，后续是否启动分组运算功能将由模块依据当前配置自动判定: 分组名={}",
             groupConfig.group_name());
  }
  return ok;
}

}  // namespace ConfigPusher
