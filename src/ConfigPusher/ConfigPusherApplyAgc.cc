#include "ConfigPusherApplyAgc.h"

#include <string>
#include "Logger.h"
#include "ProtoLogUtil.hpp"

namespace ConfigPusher {

bool applyAgcConfig(const ConfigPusherProto::AgcConfig& config,
                    AGCProto::AGCService::StubInterface* stub) {
  if (stub == nullptr) {
    LOG_ERROR("AGC gRPC stub 为空");
    return false;
  }

  bool ok = true;
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
      LOG_INFO("AGC 配置任务声明 start=true，当前版本仅保留兼容日志，不再额外调用 StartGroup: 控制组名={}",
               groupConfig.group_name());
    }
    LOG_INFO("AGC 配置任务下发完成，后续是否启动控制组功能将由模块依据当前配置自动判定: 控制组名={}",
             groupConfig.group_name());
  }
  return ok;
}

}  // namespace ConfigPusher
