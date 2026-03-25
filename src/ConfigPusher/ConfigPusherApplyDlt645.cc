#include "ConfigPusherApplyDlt645.h"

#include <google/protobuf/message.h>

#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Logger.h"
#include "ProtoLogUtil.hpp"

namespace ConfigPusher {
namespace {
constexpr std::string_view kDeviceNoPlaceholder = "{device_no}";

struct PlannedDlt645Task {
  std::string connName;
  DLT645Proto::UpsertLinkRequest linkReq;
  DLT645Proto::UpsertPointTableRequest pointTableReq;
  bool start = false;
};

bool isValidDeviceNo(const std::string &deviceNo) {
  if (deviceNo.size() != 2) {
    return false;
  }
  for (char ch : deviceNo) {
    if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
      return false;
    }
  }
  return true;
}

std::string replaceDeviceNoPlaceholder(const std::string &text, const std::string &deviceNo) {
  std::string out = text;
  auto pos = out.find(kDeviceNoPlaceholder);
  while (pos != std::string::npos) {
    out.replace(pos, kDeviceNoPlaceholder.size(), deviceNo);
    pos = out.find(kDeviceNoPlaceholder, pos + deviceNo.size());
  }
  return out;
}

std::string makeExpandedConnName(const std::string &baseConnName, const std::string &deviceNo) {
  if (baseConnName.find(kDeviceNoPlaceholder) != std::string::npos) {
    return replaceDeviceNoPlaceholder(baseConnName, deviceNo);
  }
  return baseConnName + "_" + deviceNo;
}

bool validateExpandedConnNames(const ConfigPusherProto::Dlt645LinkTask &task,
                               std::vector<std::string> *outConnNames) {
  if (outConnNames == nullptr) {
    LOG_ERROR("DLT645 配置展开失败: 输出参数为空");
    return false;
  }
  outConnNames->clear();

  if (!task.has_link() || !task.link().has_config()) {
    LOG_ERROR("DLT645 配置任务缺少 link/config");
    return false;
  }
  const auto &linkConfig = task.link().config();
  if (linkConfig.conn_name().empty()) {
    LOG_ERROR("DLT645 配置任务缺少 config.conn_name");
    return false;
  }
  if (task.device_nos_size() == 0) {
    outConnNames->push_back(linkConfig.conn_name());
    return true;
  }
  if (linkConfig.protocol_variant() != DLT645Proto::PROTOCOL_VARIANT_DLT645_PCD) {
    LOG_ERROR("DLT645 配置任务启用 device_nos 失败: conn_name={}, 原因=仅 DLT645PCD 支持批量设备序号",
              linkConfig.conn_name());
    return false;
  }

  std::unordered_set<std::string> connNames;
  connNames.reserve(task.device_nos_size());
  for (const auto &deviceNo : task.device_nos()) {
    if (!isValidDeviceNo(deviceNo)) {
      LOG_ERROR("DLT645 配置任务 device_no 非法: conn_name={}, device_no={}, 要求=2位十六进制字符串",
                linkConfig.conn_name(), deviceNo);
      return false;
    }
    auto expandedConnName = makeExpandedConnName(linkConfig.conn_name(), deviceNo);
    if (!connNames.insert(expandedConnName).second) {
      LOG_ERROR("DLT645 配置任务展开后连接名重复: 基础连接名={}, 展开连接名={}, device_no={}",
                linkConfig.conn_name(), expandedConnName, deviceNo);
      return false;
    }
    outConnNames->push_back(std::move(expandedConnName));
  }
  return true;
}

bool buildPointTableRequest(const ConfigPusherProto::Dlt645LinkTask &task,
                            const std::string &expandedConnName,
                            const std::string &deviceNo,
                            DLT645Proto::UpsertPointTableRequest *outReq) {
  if (outReq == nullptr) {
    LOG_ERROR("DLT645 点表展开失败: 输出参数为空");
    return false;
  }

  outReq->Clear();
  if (task.has_point_table()) {
    *outReq = task.point_table();
  }
  if (task.device_nos_size() == 0) {
    if (outReq->conn_name().empty()) {
      outReq->set_conn_name(expandedConnName);
    }
  } else {
    const auto &baseConnName = task.link().config().conn_name();
    if (outReq->conn_name().empty() || outReq->conn_name() == baseConnName) {
      outReq->set_conn_name(expandedConnName);
    } else if (outReq->conn_name().find(kDeviceNoPlaceholder) == std::string::npos) {
      LOG_ERROR("DLT645 点表展开失败: 启用 device_nos 时 point_table.conn_name 必须为空、等于基础 conn_name 或包含占位符 {}",
                kDeviceNoPlaceholder);
      return false;
    } else {
      outReq->set_conn_name(replaceDeviceNoPlaceholder(outReq->conn_name(), deviceNo));
    }
  }

  // 在 CONFIG_PUSHER 编排下，jsonc 是最终真相源；点表与数据块统一按全量覆盖语义收敛。
  outReq->set_replace(true);
  return true;
}

bool buildDlt645Plan(const ConfigPusherProto::Dlt645Config &config,
                     std::vector<PlannedDlt645Task> *outPlan,
                     std::unordered_set<std::string> *outDesiredConnNames) {
  if (outPlan == nullptr || outDesiredConnNames == nullptr) {
    LOG_ERROR("DLT645 配置编排失败: 输出参数为空");
    return false;
  }
  outPlan->clear();
  outDesiredConnNames->clear();

  for (const auto &task : config.links()) {
    std::vector<std::string> expandedConnNames;
    if (!validateExpandedConnNames(task, &expandedConnNames)) {
      return false;
    }
    if (task.device_nos_size() > 0) {
      LOG_INFO("DLT645 配置任务启用 device_nos 展开: 基础连接名={}, 展开数量={}",
               task.link().config().conn_name(), expandedConnNames.size());
    }

    for (size_t i = 0; i < expandedConnNames.size(); ++i) {
      const auto deviceNo = task.device_nos_size() > 0 ? task.device_nos(static_cast<int>(i)) : std::string();
      if (!outDesiredConnNames->insert(expandedConnNames[i]).second) {
        LOG_ERROR("DLT645 配置任务存在重复连接名: {}", expandedConnNames[i]);
        return false;
      }

      PlannedDlt645Task planned;
      planned.connName = expandedConnNames[i];
      planned.linkReq = task.link();
      if (!deviceNo.empty()) {
        planned.linkReq.mutable_config()->set_conn_name(planned.connName);
        planned.linkReq.mutable_config()->set_device_no(deviceNo);
      }
      if (!buildPointTableRequest(task, planned.connName, deviceNo, &planned.pointTableReq)) {
        return false;
      }
      planned.start = task.start();
      outPlan->push_back(std::move(planned));
    }
  }

  return true;
}

bool reconcileExistingDlt645Links(const std::unordered_set<std::string> &desiredConnNames,
                                  DLT645Proto::DLT645Service::StubInterface *stub) {
  DLT645Proto::Empty listReq;
  DLT645Proto::ListLinksResponse listResp;
  grpc::ClientContext listCtx;
  LOG_INFO("发送 DLT645 链路列表请求报文: {}", formatProtoForLog(listReq));
  auto status = stub->ListLinks(&listCtx, listReq, &listResp);
  if (!status.ok()) {
    LOG_ERROR("获取 DLT645 现有链路列表失败: 请求={}, 原因={}",
              formatProtoForLog(listReq),
              status.error_message());
    return false;
  }
  LOG_INFO("收到 DLT645 链路列表响应报文: {}", formatProtoForLog(listResp));

  for (const auto &existing : listResp.links()) {
    const auto &connName = existing.config().conn_name();
    if (connName.empty()) {
      LOG_WARNING("DLT645 链路列表返回空连接名，跳过本次收敛项");
      continue;
    }
    if (!desiredConnNames.contains(connName)) {
      DLT645Proto::DeleteLinkRequest req;
      req.set_conn_name(connName);
      LOG_INFO("DLT645 开始删除 jsonc 未声明的旧链路: 连接名={}", connName);
      LOG_INFO("发送 DLT645 删除链路请求报文: {}", formatProtoForLog(req));
      grpc::ClientContext ctx;
      DLT645Proto::Empty resp;
      status = stub->DeleteLink(&ctx, req, &resp);
      if (!status.ok()) {
        LOG_ERROR("DLT645 删除旧链路失败: 连接名={}, 请求={}, 原因={}",
                  connName,
                  formatProtoForLog(req),
                  status.error_message());
        return false;
      }
      LOG_INFO("收到 DLT645 删除链路响应报文: {}", formatProtoForLog(resp));
      continue;
    }

    if (existing.state() == DLT645Proto::LINK_STATE_RUNNING) {
      DLT645Proto::StopLinkRequest req;
      req.set_conn_name(connName);
      LOG_INFO("DLT645 开始停止运行中的现有链路，以便按 jsonc 覆盖配置: 连接名={}", connName);
      LOG_INFO("发送 DLT645 停止链路请求报文: {}", formatProtoForLog(req));
      grpc::ClientContext ctx;
      DLT645Proto::Empty resp;
      status = stub->StopLink(&ctx, req, &resp);
      if (!status.ok()) {
        LOG_ERROR("DLT645 停止现有链路失败: 连接名={}, 请求={}, 原因={}",
                  connName,
                  formatProtoForLog(req),
                  status.error_message());
        return false;
      }
      LOG_INFO("收到 DLT645 停止链路响应报文: {}", formatProtoForLog(resp));
    }
  }

  return true;
}
}  // namespace

bool applyDlt645Config(const ConfigPusherProto::Dlt645Config &config, DLT645Proto::DLT645Service::StubInterface *stub) {
  if (stub == nullptr) {
    LOG_ERROR("DLT645 gRPC stub 为空");
    return false;
  }

  std::vector<PlannedDlt645Task> plan;
  std::unordered_set<std::string> desiredConnNames;
  if (!buildDlt645Plan(config, &plan, &desiredConnNames)) {
    return false;
  }

  LOG_INFO("开始下发 DLT645 配置: 任务数={}", config.links_size());
  if (!reconcileExistingDlt645Links(desiredConnNames, stub)) {
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
      LOG_ERROR("DLT645 MQTT 配置下发失败: 请求={}, 原因={}", formatProtoForLog(req), status.ok() ? resp.message() : status.error_message());
      ok = false;
    } else {
      LOG_INFO("收到 DLT645 MQTT 配置响应报文: {}", formatProtoForLog(resp));
      LOG_INFO("DLT645 MQTT 配置下发成功");
    }
  } else {
    LOG_WARNING("DLT645 配置缺少 MQTT 连接参数");
  }

  for (const auto &planned : plan) {
    LOG_INFO("开始下发 DLT645 连接配置: 连接名={}", planned.connName);
    LOG_INFO("发送 DLT645 连接配置请求报文: {}", formatProtoForLog(planned.linkReq));
    DLT645Proto::LinkInfo linkInfo;
    grpc::ClientContext linkCtx;
    auto status = stub->UpsertLink(&linkCtx, planned.linkReq, &linkInfo);
    if (!status.ok()) {
      LOG_ERROR("DLT645 连接配置失败: 连接名={}, 请求={}, 原因={}",
                planned.connName, formatProtoForLog(planned.linkReq), status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 DLT645 连接配置响应报文: {}", formatProtoForLog(linkInfo));
    LOG_INFO("DLT645 连接配置成功: 连接名={}, 连接ID={}", planned.connName, linkInfo.conn_id());

    LOG_INFO("开始按全量覆盖语义下发 DLT645 点表: 连接名={}, 点数={}, 数据块数={}, 是否替换={}",
             planned.pointTableReq.conn_name(),
             planned.pointTableReq.points_size(),
             planned.pointTableReq.blocks_size(),
             planned.pointTableReq.replace());
    LOG_INFO("发送 DLT645 点表请求报文: {}", formatProtoForLog(planned.pointTableReq));
    DLT645Proto::Empty ptResp;
    grpc::ClientContext ptCtx;
    status = stub->UpsertPointTable(&ptCtx, planned.pointTableReq, &ptResp);
    if (!status.ok()) {
      LOG_ERROR("DLT645 点表下发失败: 连接名={}, 请求={}, 原因={}",
                planned.pointTableReq.conn_name(), formatProtoForLog(planned.pointTableReq), status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("收到 DLT645 点表响应报文: {}", formatProtoForLog(ptResp));
    LOG_INFO("DLT645 点表下发成功: 连接名={}, 点数={}, 数据块数={}",
             planned.pointTableReq.conn_name(),
             planned.pointTableReq.points_size(),
             planned.pointTableReq.blocks_size());

    if (planned.start) {
      LOG_INFO("DLT645 配置任务声明 start=true，当前版本仅保留兼容日志，不再额外调用 StartLink: 连接名={}",
               planned.connName);
    }
    LOG_INFO("DLT645 配置任务下发完成，后续是否启动连接功能将由模块依据当前配置自动判定: 连接名={}",
             planned.connName);
  }
  return ok;
}
}  // namespace ConfigPusher
