#include "ConfigPusherApplyDlt645.h"

#include <google/protobuf/message.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Logger.h"
#include "ProtoLogUtil.hpp"

namespace ConfigPusher {
namespace {
constexpr std::string_view kDeviceNoPlaceholder = "{device_no}";

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
  if (!task.has_point_table() ||
      (task.point_table().points_size() == 0 && task.point_table().blocks_size() == 0)) {
    return false;
  }

  *outReq = task.point_table();
  if (task.device_nos_size() == 0) {
    if (outReq->conn_name().empty()) {
      outReq->set_conn_name(expandedConnName);
    }
    return true;
  }

  const auto &baseConnName = task.link().config().conn_name();
  if (outReq->conn_name().empty() || outReq->conn_name() == baseConnName) {
    outReq->set_conn_name(expandedConnName);
    return true;
  }
  if (outReq->conn_name().find(kDeviceNoPlaceholder) == std::string::npos) {
    LOG_ERROR("DLT645 点表展开失败: 启用 device_nos 时 point_table.conn_name 必须为空、等于基础 conn_name 或包含占位符 {}",
              kDeviceNoPlaceholder);
    return false;
  }

  outReq->set_conn_name(replaceDeviceNoPlaceholder(outReq->conn_name(), deviceNo));
  return true;
}
}  // namespace

bool applyDlt645Config(const ConfigPusherProto::Dlt645Config &config, DLT645Proto::DLT645Service::StubInterface *stub) {
  struct PendingStartLink {
    std::string connName;
    DLT645Proto::StartLinkRequest request;
  };

  if (stub == nullptr) {
    LOG_ERROR("DLT645 gRPC stub 为空");
    return false;
  }

  bool ok = true;
  std::vector<PendingStartLink> pendingStarts;
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

  LOG_INFO("开始下发 DLT645 配置: 任务数={}", config.links_size());
  for (const auto &task : config.links()) {
    std::vector<std::string> expandedConnNames;
    if (!validateExpandedConnNames(task, &expandedConnNames)) {
      ok = false;
      continue;
    }
    if (task.device_nos_size() > 0) {
      LOG_INFO("DLT645 配置任务启用 device_nos 展开: 基础连接名={}, 展开数量={}",
               task.link().config().conn_name(), expandedConnNames.size());
    }

    for (size_t i = 0; i < expandedConnNames.size(); ++i) {
      DLT645Proto::UpsertLinkRequest linkReq = task.link();
      const auto deviceNo = task.device_nos_size() > 0 ? task.device_nos(static_cast<int>(i)) : std::string();
      if (!deviceNo.empty()) {
        linkReq.mutable_config()->set_conn_name(expandedConnNames[i]);
        linkReq.mutable_config()->set_device_no(deviceNo);
      }

      LOG_INFO("发送 DLT645 连接配置请求报文: {}", formatProtoForLog(linkReq));
      DLT645Proto::LinkInfo linkInfo;
      grpc::ClientContext linkCtx;
      auto status = stub->UpsertLink(&linkCtx, linkReq, &linkInfo);
      if (!status.ok()) {
        LOG_ERROR("DLT645 连接配置失败: 连接名={}, 请求={}, 原因={}",
                  expandedConnNames[i], formatProtoForLog(linkReq), status.error_message());
        ok = false;
        continue;
      }
      LOG_INFO("收到 DLT645 连接配置响应报文: {}", formatProtoForLog(linkInfo));
      LOG_INFO("DLT645 连接配置成功: 连接名={}, 连接ID={}", expandedConnNames[i], linkInfo.conn_id());

      if (task.has_point_table() &&
          (task.point_table().points_size() > 0 || task.point_table().blocks_size() > 0)) {
        DLT645Proto::UpsertPointTableRequest ptReq;
        if (!buildPointTableRequest(task, expandedConnNames[i], deviceNo, &ptReq)) {
          ok = false;
          continue;
        }
        LOG_INFO("发送 DLT645 点表请求报文: {}", formatProtoForLog(ptReq));
        DLT645Proto::Empty ptResp;
        grpc::ClientContext ptCtx;
        status = stub->UpsertPointTable(&ptCtx, ptReq, &ptResp);
        if (!status.ok()) {
          LOG_ERROR("DLT645 点表下发失败: 连接名={}, 请求={}, 原因={}",
                    ptReq.conn_name(), formatProtoForLog(ptReq), status.error_message());
          ok = false;
          continue;
        }
        LOG_INFO("收到 DLT645 点表响应报文: {}", formatProtoForLog(ptResp));
        LOG_INFO("DLT645 点表下发成功: 连接名={}, 点数={}, 数据块数={}",
                 ptReq.conn_name(),
                 ptReq.points_size(),
                 ptReq.blocks_size());
      }

      if (task.start()) {
        DLT645Proto::StartLinkRequest startReq;
        startReq.set_conn_name(expandedConnNames[i]);
        LOG_INFO("DLT645 连接已加入启动连接功能队列: 连接名={}, 请求={}",
                 expandedConnNames[i], formatProtoForLog(startReq));
        pendingStarts.push_back(PendingStartLink{expandedConnNames[i], std::move(startReq)});
      }
    }
  }

  if (pendingStarts.empty()) {
    LOG_INFO("DLT645 无需启动连接功能");
    return ok;
  }

  LOG_INFO("开始并发启动 DLT645 连接功能: 数量={}", pendingStarts.size());
  std::atomic<bool> startAllOk{true};
  std::vector<std::thread> startWorkers;
  startWorkers.reserve(pendingStarts.size());
  for (const auto &item : pendingStarts) {
    startWorkers.emplace_back([stub, &startAllOk, item]() {
      constexpr auto kStartTimeout = std::chrono::seconds(10);
      DLT645Proto::Empty startResp;
      grpc::ClientContext startCtx;
      startCtx.set_deadline(std::chrono::system_clock::now() + kStartTimeout);
      LOG_INFO("发送 DLT645 启动连接功能请求报文: 连接名={}, 请求={}", item.connName, formatProtoForLog(item.request));
      auto status = stub->StartLink(&startCtx, item.request, &startResp);
      if (!status.ok()) {
        LOG_ERROR("DLT645 启动连接功能失败: 连接名={}, 请求={}, 原因={}", item.connName, formatProtoForLog(item.request), status.error_message());
        startAllOk.store(false);
        return;
      }
      LOG_INFO("收到 DLT645 启动连接功能响应报文: 连接名={}, 响应={}", item.connName, formatProtoForLog(startResp));
      LOG_INFO("DLT645 启动连接功能成功: 连接名={}", item.connName);
    });
  }

  for (auto &worker : startWorkers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  if (!startAllOk.load()) {
    ok = false;
  }
  return ok;
}
}  // namespace ConfigPusher
