#include "ConfigPusherApplyAvc.h"

#include <unordered_set>
#include <vector>

#include <string>

#include "Logger.h"
#include "ProtoLogUtil.hpp"

namespace ConfigPusher {
namespace {
struct PlannedAvcTask {
  std::string groupName;
  AVCProto::UpsertGroupRequest upsertReq;
  bool start = false;
};

bool buildAvcPlan(const ConfigPusherProto::AvcConfig &config,
                  std::vector<PlannedAvcTask> *outPlan,
                  std::unordered_set<std::string> *outDesiredGroupNames) {
  if (outPlan == nullptr || outDesiredGroupNames == nullptr) {
    LOG_ERROR("AVC 配置编排失败: 输出参数为空");
    return false;
  }
  outPlan->clear();
  outDesiredGroupNames->clear();

  for (const auto &task : config.groups()) {
    if (!task.has_upsert() || !task.upsert().has_config()) {
      LOG_ERROR("AVC 配置任务缺少 upsert/config");
      return false;
    }
    const auto &groupConfig = task.upsert().config();
    if (groupConfig.group_name().empty()) {
      LOG_ERROR("AVC 配置任务缺少 config.group_name");
      return false;
    }
    if (!outDesiredGroupNames->insert(groupConfig.group_name()).second) {
      LOG_ERROR("AVC 配置任务存在重复控制组名: {}", groupConfig.group_name());
      return false;
    }

    PlannedAvcTask planned;
    planned.groupName = groupConfig.group_name();
    planned.upsertReq = task.upsert();
    planned.start = task.start();
    outPlan->push_back(std::move(planned));
  }

  return true;
}

bool reconcileExistingAvcGroups(const std::unordered_set<std::string> &desiredGroupNames,
                                AVCProto::AVCService::StubInterface *stub) {
  AVCProto::Empty listReq;
  AVCProto::ListGroupsResponse listResp;
  grpc::ClientContext listCtx;
  LOG_INFO("发送 AVC 控制组列表请求报文: {}", formatProtoForLog(listReq));
  auto status = stub->ListGroups(&listCtx, listReq, &listResp);
  if (!status.ok()) {
    LOG_ERROR("获取 AVC 现有控制组列表失败: 请求={}, 原因={}",
              formatProtoForLog(listReq),
              status.error_message());
    return false;
  }
  LOG_INFO("收到 AVC 控制组列表响应报文: {}", formatProtoForLog(listResp));

  for (const auto &existing : listResp.groups()) {
    const auto &groupName = existing.config().group_name();
    if (groupName.empty()) {
      LOG_WARNING("AVC 控制组列表返回空控制组名，跳过本次收敛项");
      continue;
    }
    if (!desiredGroupNames.contains(groupName)) {
      AVCProto::DeleteGroupRequest req;
      req.set_group_name(groupName);
      LOG_INFO("AVC 开始删除 jsonc 未声明的旧控制组: 控制组名={}", groupName);
      LOG_INFO("发送 AVC 删除控制组请求报文: {}", formatProtoForLog(req));
      grpc::ClientContext ctx;
      AVCProto::Empty resp;
      status = stub->DeleteGroup(&ctx, req, &resp);
      if (!status.ok()) {
        LOG_ERROR("AVC 删除旧控制组失败: 控制组名={}, 请求={}, 原因={}",
                  groupName,
                  formatProtoForLog(req),
                  status.error_message());
        return false;
      }
      LOG_INFO("收到 AVC 删除控制组响应报文: {}", formatProtoForLog(resp));
      continue;
    }

    if (existing.state() == AVCProto::GROUP_STATE_RUNNING) {
      AVCProto::StopGroupRequest req;
      req.set_group_name(groupName);
      LOG_INFO("AVC 开始停止运行中的现有控制组，以便按 jsonc 覆盖配置: 控制组名={}", groupName);
      LOG_INFO("发送 AVC 停止控制组请求报文: {}", formatProtoForLog(req));
      grpc::ClientContext ctx;
      AVCProto::Empty resp;
      status = stub->StopGroup(&ctx, req, &resp);
      if (!status.ok()) {
        LOG_ERROR("AVC 停止现有控制组失败: 控制组名={}, 请求={}, 原因={}",
                  groupName,
                  formatProtoForLog(req),
                  status.error_message());
        return false;
      }
      LOG_INFO("收到 AVC 停止控制组响应报文: {}", formatProtoForLog(resp));
    }
  }

  return true;
}
}  // namespace

bool applyAvcConfig(const ConfigPusherProto::AvcConfig &config,
                    AVCProto::AVCService::StubInterface *stub) {
  if (stub == nullptr) {
    LOG_ERROR("AVC gRPC stub 为空");
    return false;
  }

  std::vector<PlannedAvcTask> plan;
  std::unordered_set<std::string> desiredGroupNames;
  if (!buildAvcPlan(config, &plan, &desiredGroupNames)) {
    return false;
  }

  bool ok = true;
  LOG_INFO("开始下发 AVC 配置: 控制组任务数={}", config.groups_size());
  if (!reconcileExistingAvcGroups(desiredGroupNames, stub)) {
    return false;
  }

  for (const auto &planned : plan) {
    const auto &groupConfig = planned.upsertReq.config();
    LOG_INFO("处理 AVC 控制组任务: 控制组名={}, 成员数={}, 下发后是否启动控制组功能={}",
             groupConfig.group_name(),
             groupConfig.members_size(),
             planned.start);

    LOG_INFO("发送 AVC 控制组配置请求报文: {}", formatProtoForLog(planned.upsertReq));
    AVCProto::GroupInfo upsertResp;
    grpc::ClientContext upsertCtx;
    auto status = stub->UpsertGroup(&upsertCtx, planned.upsertReq, &upsertResp);
    if (!status.ok()) {
      LOG_ERROR("AVC 控制组配置失败: 控制组名={}, 请求={}, 原因={}",
                groupConfig.group_name(),
                formatProtoForLog(planned.upsertReq),
                status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 AVC 控制组配置响应报文: {}", formatProtoForLog(upsertResp));
    LOG_INFO("AVC 控制组配置成功: 控制组名={}, conn_id={}, 成员数={}",
             upsertResp.config().group_name(),
             upsertResp.conn_id(),
             groupConfig.members_size());

    if (planned.start) {
      LOG_INFO("AVC 配置任务声明 start=true，当前版本仅保留兼容日志，不再额外调用 StartGroup: 控制组名={}",
               groupConfig.group_name());
    }
    LOG_INFO("AVC 配置任务下发完成，后续是否启动控制组功能将由模块依据当前配置自动判定: 控制组名={}",
             groupConfig.group_name());
  }
  return ok;
}

}  // namespace ConfigPusher
