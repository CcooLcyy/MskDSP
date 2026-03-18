#include "ConfigPusherApplyAgc.h"

#include <string>
#include <utility>
#include <vector>

#include "Logger.h"
#include "ProtoLogUtil.hpp"

namespace ConfigPusher {

bool applyAgcConfig(const ConfigPusherProto::AgcConfig& config,
                    AGCProto::AGCService::StubInterface* stub) {
  struct PendingStartGroup {
    std::string groupName;
    AGCProto::StartGroupRequest request;
  };

  if (stub == nullptr) {
    LOG_ERROR("AGC gRPC stub 为空");
    return false;
  }

  bool ok = true;
  std::vector<PendingStartGroup> pendingStarts;
  pendingStarts.reserve(static_cast<size_t>(config.groups_size()));
  LOG_INFO("开始下发 AGC 配置: 控制组任务数={}", config.groups_size());

  for (const auto& task : config.groups()) {
    if (!task.has_upsert() || !task.upsert().has_config()) {
      LOG_ERROR("AGC 配置任务缺少 upsert/config");
      ok = false;
      continue;
    }
    const auto& groupConfig = task.upsert().config();
    if (groupConfig.group_name().empty()) {
      LOG_ERROR("AGC 配置任务缺少 config.group_name");
      ok = false;
      continue;
    }
    LOG_INFO("处理 AGC 控制组任务: 控制组名={}, 成员数={}, 下发后是否启动控制组功能={}",
             groupConfig.group_name(),
             groupConfig.members_size(),
             task.start());

    AGCProto::UpsertGroupRequest upsertReq = task.upsert();
    LOG_INFO("发送 AGC 控制组配置请求报文: {}", formatProtoForLog(upsertReq));
    AGCProto::GroupInfo upsertResp;
    grpc::ClientContext upsertCtx;
    auto status = stub->UpsertGroup(&upsertCtx, upsertReq, &upsertResp);
    if (!status.ok()) {
      LOG_ERROR("AGC 控制组配置失败: 控制组名={}, 请求={}, 原因={}",
                groupConfig.group_name(),
                formatProtoForLog(upsertReq),
                status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 AGC 控制组配置响应报文: {}", formatProtoForLog(upsertResp));
    LOG_INFO("AGC 控制组配置成功: 控制组名={}, conn_id={}, 成员数={}",
             upsertResp.config().group_name(),
             upsertResp.conn_id(),
             groupConfig.members_size());

    if (task.start()) {
      AGCProto::StartGroupRequest startReq;
      startReq.set_group_name(groupConfig.group_name());
      LOG_INFO("AGC 控制组已加入启动功能队列: 控制组名={}, 请求={}",
               groupConfig.group_name(),
               formatProtoForLog(startReq));
      pendingStarts.push_back(PendingStartGroup{groupConfig.group_name(), std::move(startReq)});
    }
  }

  if (pendingStarts.empty()) {
    LOG_INFO("AGC 无需启动控制组功能");
    return ok;
  }

  LOG_INFO("开始启动 AGC 控制组功能: 数量={}", pendingStarts.size());
  for (const auto& item : pendingStarts) {
    AGCProto::Empty startResp;
    grpc::ClientContext startCtx;
    LOG_INFO("发送 AGC 启动控制组功能请求报文: 控制组名={}, 请求={}",
             item.groupName,
             formatProtoForLog(item.request));
    auto status = stub->StartGroup(&startCtx, item.request, &startResp);
    if (!status.ok()) {
      LOG_ERROR("AGC 启动控制组功能失败: 控制组名={}, 请求={}, 原因={}",
                item.groupName,
                formatProtoForLog(item.request),
                status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 AGC 启动控制组功能响应报文: 控制组名={}, 响应={}",
             item.groupName,
             formatProtoForLog(startResp));
    LOG_INFO("AGC 启动控制组功能成功: 控制组名={}", item.groupName);
  }
  return ok;
}

}  // namespace ConfigPusher
