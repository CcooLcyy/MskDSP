#include "IEC104LinkManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Logger.h"
#include "IEC104LibInfo.h"
#include "ThreadUtil.hpp"

namespace IEC104 {
namespace {
constexpr uint8_t kIec104QualityGood = 0x00;
constexpr uint8_t kIec104QualityInvalid = 0x80;
constexpr uint32_t kSynchronousCommandTimeoutMs = 8000;

constexpr uint8_t kCotSpontaneous = 3;

DataCenterProto::Quality toDataCenterQuality(uint8_t qds) {
  if ((qds & kIec104QualityInvalid) != 0) {
    return DataCenterProto::QUALITY_BAD;
  }
  if (qds == 0) {
    return DataCenterProto::QUALITY_GOOD;
  }
  return DataCenterProto::QUALITY_UNCERTAIN;
}

uint8_t toIec104Quality(DataCenterProto::Quality q) {
  switch (q) {
  case DataCenterProto::QUALITY_GOOD:
    return kIec104QualityGood;
  case DataCenterProto::QUALITY_BAD:
  case DataCenterProto::QUALITY_UNCERTAIN:
  case DataCenterProto::QUALITY_UNSPECIFIED:
  default:
  return kIec104QualityInvalid;
}
}

double applyScale(double raw, double scale, double offset) {
  if (scale == 0.0) {
    scale = 1.0;
  }
  return raw * scale + offset;
}

bool reverseScale(double eng, double scale, double offset, double* out) {
  if (out == nullptr) {
    return false;
  }
  if (scale == 0.0) {
    scale = 1.0;
  }
  const double value = (eng - offset) / scale;
  if (!std::isfinite(value)) {
    return false;
  }
  *out = value;
  return true;
}

bool shouldReport(double value, double deadband, const std::optional<double>& last) {
  if (deadband <= 0 || !last.has_value()) {
    return true;
  }
  return std::fabs(value - last.value()) >= deadband;
}

bool pointValueToDouble(const DataCenterProto::PointValue& v, double* out) {
  if (out == nullptr) {
    return false;
  }
  switch (v.kind_case()) {
  case DataCenterProto::PointValue::kDoubleValue:
    *out = v.double_value();
    return true;
  case DataCenterProto::PointValue::kIntValue:
    *out = static_cast<double>(v.int_value());
    return true;
  case DataCenterProto::PointValue::kBoolValue:
    *out = v.bool_value() ? 1.0 : 0.0;
    return true;
  default:
    return false;
  }
}

bool pointValueToBool(const DataCenterProto::PointValue& v, bool* out) {
  if (out == nullptr) {
    return false;
  }
  switch (v.kind_case()) {
  case DataCenterProto::PointValue::kBoolValue:
    *out = v.bool_value();
    return true;
  case DataCenterProto::PointValue::kIntValue:
    *out = (v.int_value() != 0);
    return true;
  case DataCenterProto::PointValue::kDoubleValue:
    *out = (v.double_value() != 0.0);
    return true;
  default:
    return false;
  }
}

grpc::Status makeNotFound(const std::string& connName) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::format("未找到链路: {}", connName));
}

const char* commandStatusToString(DataCenterProto::CommandStatus status) {
  switch (status) {
  case DataCenterProto::COMMAND_ACCEPTED:
    return "已接受";
  case DataCenterProto::COMMAND_REJECTED:
    return "已拒绝";
  case DataCenterProto::COMMAND_NO_ROUTE:
    return "无路由";
  case DataCenterProto::COMMAND_AMBIGUOUS_ROUTE:
    return "多路由";
  case DataCenterProto::COMMAND_TARGET_UNAVAILABLE:
    return "目标不可用";
  case DataCenterProto::COMMAND_TIMEOUT:
    return "超时";
  case DataCenterProto::COMMAND_INTERNAL_ERROR:
    return "内部错误";
  case DataCenterProto::COMMAND_STATUS_UNSPECIFIED:
  default:
    return "未指定";
  }
}

CommandResult rejectCommand(std::string reason) {
  CommandResult result;
  result.accepted = false;
  result.reason = std::move(reason);
  return result;
}
}  // namespace

void LinkManager::stopDataCenterSubscribeLocked(LinkRuntime* link) {
  if (link == nullptr) {
    return;
  }
  if (link->dcSubscribeThread.joinable()) {
    LOG_INFO("IEC104 停止 DataCenter 订阅: conn_name={}", link->config.conn_name());
    link->dcSubscribeThread.request_stop();
    link->dcSubscribeThread.join();
  }
  link->dcSubscribeContext.reset();
}

void LinkManager::startDataCenterSubscribeLocked(const std::string& connName, LinkRuntime* link) {
  if (link == nullptr || !isSlaveStation(link->config) || !link->transport) {
    return;
  }
  stopDataCenterSubscribeLocked(link);

  auto tags = link->pointTable.Tags();
  struct PointMeta {
    uint32_t ioa = 0;
    IEC104Proto::PointType type = IEC104Proto::POINT_TYPE_UNSPECIFIED;
    double scale = 1.0;
    double offset = 0.0;
    double deadband = 0.0;
  };
  std::unordered_map<std::string, PointMeta> metaByTag;
  metaByTag.reserve(tags.size());
  for (const auto& tag : tags) {
    auto p = link->pointTable.FindByTag(tag);
    if (p) {
      metaByTag.emplace(tag, PointMeta{p->ioa, p->type, p->scale, p->offset, p->deadband});
    }
  }

  auto* transport = link->transport.get();
  auto connId = link->connId;

  LOG_INFO("IEC104 启动 DataCenter 订阅: conn_name={}, conn_id={}, tags={}", connName, connId, tags.size());

  link->dcSubscribeContext = std::make_shared<grpc::ClientContext>();
  auto ctx = link->dcSubscribeContext;

  link->dcSubscribeThread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [this, connName, ctx, connId, tags, metaByTag, transport](std::stop_token st) {
    std::stop_callback cb(st, [&ctx]() { ctx->TryCancel(); });

    auto reader = dataCenter_.Subscribe(ctx.get(), connId, tags, false);
    if (!reader) {
      LOG_ERROR("IEC104 创建 DataCenter 订阅失败: conn_name={}, conn_id={}, tags={}", connName, connId, tags.size());
      return;
    }

    std::unordered_map<std::string, double> lastSentByTag;
    lastSentByTag.reserve(metaByTag.size());
    DataCenterProto::PointUpdate update;
    while (reader->Read(&update)) {
      if (isSimulationValueActive(connName, update.dst_tag())) {
        LOG_DEBUG("IEC104 当前 Tag 存在模拟值，忽略 DataCenter 实时值: conn_name={}, tag={}", connName, update.dst_tag());
        continue;
      }
      auto it = metaByTag.find(update.dst_tag());
      if (it == metaByTag.end()) {
        continue;
      }
      if (it->second.type == IEC104Proto::POINT_TYPE_FLOAT) {
        double value = 0;
        if (!pointValueToDouble(update.value(), &value)) {
          LOG_DEBUG("IEC104 遥测点值类型不匹配: conn_name={}, tag={}", connName, update.dst_tag());
          continue;
        }
        std::optional<double> last;
        auto lastIt = lastSentByTag.find(update.dst_tag());
        if (lastIt != lastSentByTag.end()) {
          last = lastIt->second;
        }
        if (!shouldReport(value, it->second.deadband, last)) {
          LOG_DEBUG("IEC104 死区过滤上送: conn_name={}, tag={}, value={}, last={}, 死区={}",
                    connName,
                    update.dst_tag(),
                    value,
                    last.value(),
                    it->second.deadband);
          continue;
        }
        double rawValue = 0;
        if (!reverseScale(value, it->second.scale, it->second.offset, &rawValue)) {
          LOG_WARNING("IEC104 点值反向缩放失败: conn_name={}, tag={}, value={}", connName, update.dst_tag(), value);
          continue;
        }
        PointValue pv;
        pv.ioa = it->second.ioa;
        pv.type = IEC104Proto::POINT_TYPE_FLOAT;
        pv.doubleValue = rawValue;
        pv.quality = toIec104Quality(update.quality());
        pv.tsMs = update.ts_ms();
        transport->SendPointValue(pv, kCotSpontaneous);
        lastSentByTag[update.dst_tag()] = value;
      } else if (it->second.type == IEC104Proto::POINT_TYPE_SINGLE) {
        bool value = false;
        if (!pointValueToBool(update.value(), &value)) {
          LOG_DEBUG("IEC104 单点点值类型不匹配: conn_name={}, tag={}", connName, update.dst_tag());
          continue;
        }
        PointValue pv;
        pv.ioa = it->second.ioa;
        pv.type = IEC104Proto::POINT_TYPE_SINGLE;
        pv.boolValue = value;
        pv.quality = toIec104Quality(update.quality());
        pv.tsMs = update.ts_ms();
        transport->SendPointValue(pv, kCotSpontaneous);
      }
    }

    auto finishStatus = reader->Finish();
    if (!finishStatus.ok() && !st.stop_requested()) {
      LOG_WARNING("IEC104 DataCenter 订阅异常结束: conn_name={}, conn_id={}, 错误={}",
                  connName,
                  connId,
                  finishStatus.error_message());
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastError = finishStatus.error_message();
      }
    }
  });
}

bool LinkManager::isSimulationValueActive(const std::string& connName, const std::string& tag) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  return it != linksByName_.end() && it->second.simulationValues.contains(tag);
}

void LinkManager::stopTimeSyncSubscribeLocked(LinkRuntime* link) {
  if (link == nullptr) {
    return;
  }
  if (link->dcTimeSyncThread.joinable()) {
    LOG_INFO("IEC104 停止对时订阅: conn_name={}", link->config.conn_name());
    link->dcTimeSyncThread.request_stop();
    link->dcTimeSyncThread.join();
  }
  link->dcTimeSyncContext.reset();
}

void LinkManager::startTimeSyncSubscribeLocked(const std::string& connName, LinkRuntime* link) {
  if (link == nullptr || !isMasterStation(link->config) || !link->transport) {
    return;
  }
  stopTimeSyncSubscribeLocked(link);

  const auto timeSyncTag = normalizeTimeSyncTag(link->config);
  if (timeSyncTag.empty()) {
    LOG_WARNING("IEC104 对时订阅缺少 tag: conn_name={}", connName);
    return;
  }

  std::vector<std::string> tags{timeSyncTag};
  auto* transport = link->transport.get();
  auto connId = link->connId;

  LOG_INFO("IEC104 启动对时订阅: conn_name={}, conn_id={}, tag={}", connName, connId, timeSyncTag);

  link->dcTimeSyncContext = std::make_shared<grpc::ClientContext>();
  auto ctx = link->dcTimeSyncContext;

  link->dcTimeSyncThread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [this, connName, ctx, connId, tags, transport](std::stop_token st) {
    std::stop_callback cb(st, [&ctx]() { ctx->TryCancel(); });

    auto reader = dataCenter_.Subscribe(ctx.get(), connId, tags, false);
    if (!reader) {
      LOG_ERROR("IEC104 创建对时订阅失败: conn_name={}, conn_id={}", connName, connId);
      return;
    }

    DataCenterProto::PointUpdate update;
    while (reader->Read(&update)) {
      int64_t tsMs = update.ts_ms();
      if (tsMs <= 0) {
        switch (update.value().kind_case()) {
        case DataCenterProto::PointValue::kIntValue:
          tsMs = update.value().int_value();
          break;
        case DataCenterProto::PointValue::kDoubleValue:
          tsMs = static_cast<int64_t>(update.value().double_value());
          break;
        default:
          break;
        }
      }
      if (tsMs <= 0) {
        auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
        tsMs = now.time_since_epoch().count();
      }
      LOG_INFO("IEC104 对时触发: conn_name={}, ts_ms={}", connName, tsMs);
      transport->SendTimeSync(tsMs);
    }

    auto finishStatus = reader->Finish();
    if (!finishStatus.ok() && !st.stop_requested()) {
      LOG_WARNING("IEC104 对时订阅异常结束: conn_name={}, conn_id={}, 错误={}",
                  connName,
                  connId,
                  finishStatus.error_message());
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastError = finishStatus.error_message();
      }
    }
  });
}

void LinkManager::stopCommandSubscribeLocked(LinkRuntime* link) {
  if (link == nullptr) {
    return;
  }
  if (link->dcCommandThread.joinable()) {
    LOG_INFO("IEC104 停止命令订阅: conn_name={}", link->config.conn_name());
    link->dcCommandThread.request_stop();
    link->dcCommandThread.join();
  }
  link->dcCommandContext.reset();
}

void LinkManager::startCommandSubscribeLocked(const std::string& connName, LinkRuntime* link) {
  if (link == nullptr || !isMasterStation(link->config) || !link->transport) {
    return;
  }
  stopCommandSubscribeLocked(link);

  auto tags = link->pointTable.Tags();
  const auto timeSyncTag = normalizeTimeSyncTag(link->config);
  if (!timeSyncTag.empty()) {
    tags.erase(std::remove(tags.begin(), tags.end(), timeSyncTag), tags.end());
  }
  if (tags.empty()) {
    LOG_INFO("IEC104 命令订阅无可用点: conn_name={}", connName);
    return;
  }

  struct PointMeta {
    uint32_t ioa = 0;
    IEC104Proto::PointType type = IEC104Proto::POINT_TYPE_UNSPECIFIED;
    double scale = 1.0;
    double offset = 0.0;
  };
  std::unordered_map<std::string, PointMeta> metaByTag;
  metaByTag.reserve(tags.size());
  for (const auto& tag : tags) {
    auto p = link->pointTable.FindByTag(tag);
    if (p) {
      metaByTag.emplace(tag, PointMeta{p->ioa, p->type, p->scale, p->offset});
    }
  }

  auto* transport = link->transport.get();
  auto connId = link->connId;

  LOG_INFO("IEC104 启动命令订阅: conn_name={}, conn_id={}, tags={}", connName, connId, tags.size());

  link->dcCommandContext = std::make_shared<grpc::ClientContext>();
  auto ctx = link->dcCommandContext;

  link->dcCommandThread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [this, connName, ctx, connId, tags, metaByTag, transport](std::stop_token st) {
    std::stop_callback cb(st, [&ctx]() { ctx->TryCancel(); });

    auto reader = dataCenter_.Subscribe(ctx.get(), connId, tags, false);
    if (!reader) {
      LOG_ERROR("IEC104 创建命令订阅失败: conn_name={}, conn_id={}, tags={}", connName, connId, tags.size());
      return;
    }

    DataCenterProto::PointUpdate update;
    while (reader->Read(&update)) {
      if (update.src_conn_id() == connId) {
        continue;
      }
      auto it = metaByTag.find(update.dst_tag());
      if (it == metaByTag.end()) {
        continue;
      }
      if (it->second.type == IEC104Proto::POINT_TYPE_FLOAT) {
        double value = 0;
        if (!pointValueToDouble(update.value(), &value)) {
          LOG_DEBUG("IEC104 设点点值类型不匹配: conn_name={}, tag={}", connName, update.dst_tag());
          continue;
        }
        double rawValue = 0;
        if (!reverseScale(value, it->second.scale, it->second.offset, &rawValue)) {
          LOG_WARNING("IEC104 设点反向缩放失败: conn_name={}, tag={}, value={}", connName, update.dst_tag(), value);
          continue;
        }
        LOG_INFO("IEC104 触发设点命令: conn_name={}, tag={}, ioa={}, value={}", connName, update.dst_tag(), it->second.ioa, value);
        transport->SendSetpointCommand(it->second.ioa, rawValue);
      } else if (it->second.type == IEC104Proto::POINT_TYPE_SINGLE) {
        bool value = false;
        if (!pointValueToBool(update.value(), &value)) {
          LOG_DEBUG("IEC104 遥控点值类型不匹配: conn_name={}, tag={}", connName, update.dst_tag());
          continue;
        }
        LOG_INFO("IEC104 触发遥控命令: conn_name={}, tag={}, ioa={}, value={}", connName, update.dst_tag(), it->second.ioa, value);
        transport->SendSingleCommand(it->second.ioa, value, true);
      }
    }

    auto finishStatus = reader->Finish();
    if (!finishStatus.ok() && !st.stop_requested()) {
      LOG_WARNING("IEC104 命令订阅异常结束: conn_name={}, conn_id={}, 错误={}",
                  connName,
                  connId,
                  finishStatus.error_message());
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastError = finishStatus.error_message();
      }
    }
  });
}

grpc::Status LinkManager::handleClientPointValue(const std::string& connName, const PointValue& pv) {
  uint32_t connId = 0;
  std::string tag;
  IEC104Proto::PointType type = IEC104Proto::POINT_TYPE_UNSPECIFIED;
  double scale = 1.0;
  double offset = 0.0;
  double deadband = 0.0;
  std::optional<double> last;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    connId = it->second.connId;
    auto p = it->second.pointTable.FindByIoa(pv.ioa);
    if (!p) {
      return grpc::Status::OK;
    }
    tag = p->tag;
    type = p->type;
    scale = p->scale;
    offset = p->offset;
    deadband = p->deadband;
    auto lastIt = it->second.lastReportedByTag.find(tag);
    if (lastIt != it->second.lastReportedByTag.end()) {
      last = lastIt->second;
    }
  }

  if (pv.type != IEC104Proto::POINT_TYPE_UNSPECIFIED && pv.type != type) {
    LOG_WARNING("IEC104 点类型不一致: conn_name={}, tag={}, 配置类型={}, 实际类型={}",
                connName,
                tag,
                static_cast<int>(type),
                static_cast<int>(pv.type));
    return grpc::Status::OK;
  }

  if (type == IEC104Proto::POINT_TYPE_FLOAT) {
    auto quality = toDataCenterQuality(pv.quality);
    const double engValue = applyScale(pv.doubleValue, scale, offset);
    if (!shouldReport(engValue, deadband, last)) {
      LOG_DEBUG("IEC104 死区过滤上报: conn_name={}, tag={}, value={}, last={}, 死区={}",
                connName,
                tag,
                engValue,
                last.value(),
                deadband);
      return grpc::Status::OK;
    }
    auto st = dataCenter_.PublishDouble(connId, tag, engValue, quality, pv.tsMs);
    if (!st.ok()) {
      LOG_WARNING("IEC104 发布点位失败: conn_name={}, tag={}, 错误={}", connName, tag, st.error_message());
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastError = st.error_message();
      }
    } else {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastReportedByTag[tag] = engValue;
      }
    }
    return st;
  }
  if (type == IEC104Proto::POINT_TYPE_SINGLE) {
    DataCenterProto::Quality quality = DataCenterProto::QUALITY_GOOD;
    if ((pv.quality & kIec104QualityInvalid) != 0) {
      quality = DataCenterProto::QUALITY_BAD;
    }
    auto st = dataCenter_.PublishBool(connId, tag, pv.boolValue, quality, pv.tsMs);
    if (!st.ok()) {
      LOG_WARNING("IEC104 发布点位失败: conn_name={}, tag={}, 错误={}", connName, tag, st.error_message());
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastError = st.error_message();
      }
    }
    return st;
  }

  LOG_WARNING("IEC104 点类型不匹配: conn_name={}, tag={}, type={}", connName, tag, static_cast<int>(type));
  return grpc::Status::OK;
}

CommandResult LinkManager::handleCommandValue(const std::string& connName, const CommandValue& cv) {
  uint32_t connId = 0;
  std::string tag;
  IEC104Proto::PointType type = IEC104Proto::POINT_TYPE_UNSPECIFIED;
  double scale = 1.0;
  double offset = 0.0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return rejectCommand(std::format("未找到链路: {}", connName));
    }
    if (!isSlaveStation(it->second.config)) {
      LOG_INFO("IEC104 非从站收到命令，忽略发布: conn_name={}", connName);
      return CommandResult{};
    }
    connId = it->second.connId;
    auto p = it->second.pointTable.FindByIoa(cv.ioa);
    if (!p) {
      return rejectCommand(std::format("点表未找到 IOA: {}", cv.ioa));
    }
    tag = p->tag;
    type = p->type;
    scale = p->scale;
    offset = p->offset;
  }

  if (cv.type != IEC104Proto::POINT_TYPE_UNSPECIFIED && cv.type != type) {
    LOG_WARNING("IEC104 命令点类型不一致: conn_name={}, tag={}, 配置类型={}, 实际类型={}",
                connName,
                tag,
                static_cast<int>(type),
                static_cast<int>(cv.type));
    return rejectCommand("IEC104 命令点类型不一致");
  }

  DataCenterProto::ExecuteCommandRequest req;
  req.mutable_src()->set_conn_id(connId);
  req.mutable_src()->set_tag(tag);
  req.set_quality(DataCenterProto::QUALITY_GOOD);
  req.set_timeout_ms(kSynchronousCommandTimeoutMs);
  req.set_request_id(std::format("IEC104:{}:{}", connName, cv.ioa));

  if (type == IEC104Proto::POINT_TYPE_FLOAT) {
    const double engValue = applyScale(cv.doubleValue, scale, offset);
    req.mutable_value()->set_double_value(engValue);
    DataCenterProto::ExecuteCommandResponse resp;
    auto st = dataCenter_.ExecuteCommand(req, &resp);
    if (!st.ok()) {
      LOG_WARNING("IEC104 同步执行设点失败: conn_name={}, tag={}, 错误={}", connName, tag, st.error_message());
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastError = st.error_message();
      }
      return rejectCommand(st.error_message());
    }
    if (resp.status() != DataCenterProto::COMMAND_ACCEPTED) {
      auto reason = resp.reason().empty() ? commandStatusToString(resp.status()) : resp.reason();
      LOG_WARNING("IEC104 设点被同步命令链路拒绝: conn_name={}, tag={}, value={}, status={}, reject_code={}, 原因={}",
                  connName,
                  tag,
                  engValue,
                  static_cast<int>(resp.status()),
                  static_cast<int>(resp.reject_code()),
                  reason);
      return rejectCommand(reason);
    }
    LOG_INFO("IEC104 设点同步执行成功: conn_name={}, tag={}, value={}", connName, tag, engValue);
    return CommandResult{};
  }

  if (type == IEC104Proto::POINT_TYPE_SINGLE) {
    req.mutable_value()->set_bool_value(cv.boolValue);
    DataCenterProto::ExecuteCommandResponse resp;
    auto st = dataCenter_.ExecuteCommand(req, &resp);
    if (!st.ok()) {
      LOG_WARNING("IEC104 同步执行遥控失败: conn_name={}, tag={}, 错误={}", connName, tag, st.error_message());
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastError = st.error_message();
      }
      return rejectCommand(st.error_message());
    }
    if (resp.status() != DataCenterProto::COMMAND_ACCEPTED) {
      auto reason = resp.reason().empty() ? commandStatusToString(resp.status()) : resp.reason();
      LOG_WARNING("IEC104 遥控被同步命令链路拒绝: conn_name={}, tag={}, value={}, status={}, reject_code={}, 原因={}",
                  connName,
                  tag,
                  cv.boolValue,
                  static_cast<int>(resp.status()),
                  static_cast<int>(resp.reject_code()),
                  reason);
      return rejectCommand(reason);
    }
    LOG_INFO("IEC104 遥控同步执行成功: conn_name={}, tag={}, value={}", connName, tag, cv.boolValue);
    return CommandResult{};
  }

  LOG_WARNING("IEC104 命令点类型不匹配: conn_name={}, tag={}, type={}", connName, tag, static_cast<int>(type));
  return rejectCommand("IEC104 点类型不支持命令执行");
}

grpc::Status LinkManager::handleTimeSyncCommand(const std::string& connName, int64_t tsMs) {
  if (tsMs <= 0) {
    LOG_WARNING("IEC104 对时时间戳无效: conn_name={}", connName);
    return grpc::Status::OK;
  }

  uint32_t connId = 0;
  std::string tag;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    if (!isSlaveStation(it->second.config)) {
      LOG_INFO("IEC104 非从站收到对时命令，忽略发布: conn_name={}", connName);
      return grpc::Status::OK;
    }
    connId = it->second.connId;
    tag = normalizeTimeSyncTag(it->second.config);
  }

  if (tag.empty()) {
    LOG_WARNING("IEC104 对时 tag 为空: conn_name={}", connName);
    return grpc::Status::OK;
  }

  auto st = dataCenter_.PublishInt64(connId, tag, tsMs, DataCenterProto::QUALITY_GOOD, tsMs);
  if (!st.ok()) {
    LOG_WARNING("IEC104 发布对时事件失败: conn_name={}, tag={}, 错误={}", connName, tag, st.error_message());
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end()) {
      it->second.lastError = st.error_message();
    }
  } else {
    LOG_INFO("IEC104 已发布对时事件: conn_name={}, tag={}, ts_ms={}", connName, tag, tsMs);
  }
  return st;
}

std::vector<PointValue> LinkManager::buildInterrogationSnapshot(const std::string& connName) {
  uint32_t connId = 0;
  struct PointMeta {
    uint32_t ioa = 0;
    IEC104Proto::PointType type = IEC104Proto::POINT_TYPE_UNSPECIFIED;
    double scale = 1.0;
    double offset = 0.0;
  };
  std::unordered_map<std::string, PointMeta> metaByTag;
  std::unordered_map<std::string, IEC104Proto::SimulationPoint> simulationValues;
  std::vector<std::string> tags;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return {};
    }
    connId = it->second.connId;
    tags = it->second.pointTable.Tags();
    metaByTag.reserve(tags.size());
    for (const auto& tag : tags) {
      auto p = it->second.pointTable.FindByTag(tag);
      if (p) {
        metaByTag.emplace(tag, PointMeta{p->ioa, p->type, p->scale, p->offset});
      }
    }
    simulationValues = it->second.simulationValues;
  }

  DataCenterProto::GetLatestResponse resp;
  auto status = dataCenter_.GetLatest(connId, tags, &resp);
  if (!status.ok()) {
    LOG_WARNING("IEC104 查询快照失败: conn_name={}, 错误={}", connName, status.error_message());
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end()) {
      it->second.lastError = status.error_message();
    }
    resp.Clear();
  }

  std::unordered_map<std::string, PointValue> valuesByTag;
  valuesByTag.reserve(static_cast<size_t>(resp.updates_size()) + simulationValues.size());
  for (const auto& update : resp.updates()) {
    auto it = metaByTag.find(update.dst_tag());
    if (it == metaByTag.end()) {
      continue;
    }
    if (it->second.type == IEC104Proto::POINT_TYPE_FLOAT) {
      double value = 0;
      if (!pointValueToDouble(update.value(), &value)) {
        continue;
      }
      double rawValue = 0;
      if (!reverseScale(value, it->second.scale, it->second.offset, &rawValue)) {
        LOG_WARNING("IEC104 总召点值反向缩放失败: conn_name={}, tag={}, value={}", connName, update.dst_tag(), value);
        continue;
      }
      PointValue mv;
      mv.ioa = it->second.ioa;
      mv.type = IEC104Proto::POINT_TYPE_FLOAT;
      mv.doubleValue = rawValue;
      mv.quality = toIec104Quality(update.quality());
      mv.tsMs = update.ts_ms();
      valuesByTag[update.dst_tag()] = std::move(mv);
    } else if (it->second.type == IEC104Proto::POINT_TYPE_SINGLE) {
      bool value = false;
      if (!pointValueToBool(update.value(), &value)) {
        continue;
      }
      PointValue pv;
      pv.ioa = it->second.ioa;
      pv.type = IEC104Proto::POINT_TYPE_SINGLE;
      pv.boolValue = value;
      pv.quality = toIec104Quality(update.quality());
      pv.tsMs = update.ts_ms();
      valuesByTag[update.dst_tag()] = std::move(pv);
    }
  }

  for (const auto& tag : tags) {
    auto simIt = simulationValues.find(tag);
    auto metaIt = metaByTag.find(tag);
    if (simIt == simulationValues.end() || metaIt == metaByTag.end()) {
      continue;
    }
    const auto& sim = simIt->second;
    PointValue pv;
    pv.ioa = metaIt->second.ioa;
    pv.type = metaIt->second.type;
    pv.quality = static_cast<uint8_t>(sim.quality());
    pv.tsMs = sim.ts_ms();
    if (metaIt->second.type == IEC104Proto::POINT_TYPE_FLOAT && sim.has_double_value()) {
      if (!reverseScale(sim.double_value(), metaIt->second.scale, metaIt->second.offset, &pv.doubleValue)) {
        continue;
      }
    } else if (metaIt->second.type == IEC104Proto::POINT_TYPE_SINGLE && sim.has_bool_value()) {
      pv.boolValue = sim.bool_value();
    } else {
      continue;
    }
    valuesByTag[tag] = std::move(pv);
  }

  std::vector<PointValue> out;
  out.reserve(valuesByTag.size());
  for (const auto& tag : tags) {
    auto it = valuesByTag.find(tag);
    if (it != valuesByTag.end()) {
      out.emplace_back(it->second);
    }
  }
  return out;
}

grpc::Status LinkManager::fillSimulationSnapshotLocked(
    const LinkRuntime& link, IEC104Proto::SimulationSnapshot* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  out->Clear();
  out->set_conn_name(link.config.conn_name());
  auto tags = link.pointTable.Tags();
  std::stable_sort(tags.begin(), tags.end(), [&link](const std::string& lhs, const std::string& rhs) {
    const auto left = link.pointTable.FindByTag(lhs);
    const auto right = link.pointTable.FindByTag(rhs);
    if (!left || !right) {
      return lhs < rhs;
    }
    return left->ioa < right->ioa;
  });
  for (const auto& tag : tags) {
    auto it = link.simulationValues.find(tag);
    if (it != link.simulationValues.end()) {
      *out->add_points() = it->second;
    }
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::GenerateSimulationValues(
    const IEC104Proto::SimulationRequest& request, IEC104Proto::SimulationSnapshot* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  const auto& connName = request.conn_name();
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }
  const bool incremental = request.mode() == IEC104Proto::SIMULATION_MODE_INCREMENT;
  const auto boolMode = request.bool_mode();
  constexpr double kIncrementStartValue = 1.0;
  constexpr double kIncrementStep = 1.0;

  struct SimulationPointMeta {
    std::string tag;
    PointTable::Point point;
  };
  std::vector<SimulationPointMeta> pointMetas;
  std::unordered_map<std::string, IEC104Proto::SimulationPoint> previousValues;
  uint32_t connId = 0;
  size_t configuredPointCount = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    if (!isSlaveStation(it->second.config)) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "仅允许 IEC104 从站生成模拟值");
    }
    if (it->second.pointTable.Tags().empty()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "点表未配置，无法生成模拟值");
    }

    connId = it->second.connId;
    previousValues = it->second.simulationValues;
    const auto pointTags = it->second.pointTable.Tags();
    configuredPointCount = pointTags.size();
    pointMetas.reserve(configuredPointCount);
    for (const auto& tag : pointTags) {
      auto point = it->second.pointTable.FindByTag(tag);
      if (point && PointTable::IsSimulationBusinessType(point->businessType)) {
        pointMetas.push_back(SimulationPointMeta{tag, std::move(*point)});
      }
    }
    if (pointMetas.empty()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "点表中没有可模拟的遥信或遥测点");
    }
  }

  std::unordered_map<std::string, bool> currentBoolValues;
  if (boolMode == IEC104Proto::SIMULATION_BOOL_MODE_INVERT_CURRENT) {
    std::vector<std::string> missingTags;
    for (const auto& meta : pointMetas) {
      if (meta.point.type != IEC104Proto::POINT_TYPE_SINGLE) {
        continue;
      }
      auto previousIt = previousValues.find(meta.tag);
      if (previousIt != previousValues.end() && previousIt->second.has_bool_value()) {
        currentBoolValues.emplace(meta.tag, previousIt->second.bool_value());
      } else {
        missingTags.push_back(meta.tag);
      }
    }

    if (!missingTags.empty()) {
      if (connId == 0) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "没有可取反的当前遥信值");
      }
      DataCenterProto::GetLatestResponse latest;
      status = dataCenter_.GetLatest(connId, missingTags, &latest);
      if (!status.ok()) {
        LOG_WARNING("IEC104 读取当前遥信值失败: conn_name={}, 错误={}", connName, status.error_message());
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            std::format("无法读取当前遥信值: {}", status.error_message()));
      }
      for (const auto& update : latest.updates()) {
        bool value = false;
        if (pointValueToBool(update.value(), &value)) {
          currentBoolValues[update.dst_tag()] = value;
        }
      }
    }

    for (const auto& tag : missingTags) {
      if (!currentBoolValues.contains(tag)) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            std::format("遥信 {} 没有可取反的当前值", tag));
      }
    }
  }

  std::mt19937_64 rng(std::random_device{}());
  std::uniform_real_distribution<double> floatDistribution(0.0, 100.0);
  std::bernoulli_distribution boolDistribution(0.5);
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  if (incremental) {
    std::stable_sort(pointMetas.begin(), pointMetas.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.point.ioa < rhs.point.ioa;
    });
  }

  std::unordered_map<std::string, IEC104Proto::SimulationPoint> generatedValues;
  generatedValues.reserve(pointMetas.size());
  size_t floatIndex = 0;
  for (const auto& meta : pointMetas) {
    const auto& tag = meta.tag;
    const auto& point = meta.point;
    IEC104Proto::SimulationPoint sim;
    sim.set_tag(tag);
    sim.set_type(point.type);
    sim.set_quality(0);
    sim.set_ts_ms(now);
    if (point.type == IEC104Proto::POINT_TYPE_FLOAT) {
      if (incremental) {
        sim.set_double_value(kIncrementStartValue + static_cast<double>(floatIndex) * kIncrementStep);
      } else {
        sim.set_double_value(floatDistribution(rng));
      }
      ++floatIndex;
    } else if (point.type == IEC104Proto::POINT_TYPE_SINGLE) {
      bool value = false;
      switch (boolMode) {
      case IEC104Proto::SIMULATION_BOOL_MODE_ALL_FALSE:
        value = false;
        break;
      case IEC104Proto::SIMULATION_BOOL_MODE_ALL_TRUE:
        value = true;
        break;
      case IEC104Proto::SIMULATION_BOOL_MODE_INVERT_CURRENT:
        value = !currentBoolValues.at(tag);
        break;
      case IEC104Proto::SIMULATION_BOOL_MODE_RANDOM:
      default:
        value = boolDistribution(rng);
        break;
      }
      sim.set_bool_value(value);
    } else {
      continue;
    }
    generatedValues.emplace(tag, std::move(sim));
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  if (!isSlaveStation(it->second.config)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "仅允许 IEC104 从站生成模拟值");
  }
  size_t currentSimulationPointCount = 0;
  for (const auto& tag : it->second.pointTable.Tags()) {
    const auto currentPoint = it->second.pointTable.FindByTag(tag);
    if (currentPoint && PointTable::IsSimulationBusinessType(currentPoint->businessType)) {
      ++currentSimulationPointCount;
    }
  }
  if (currentSimulationPointCount != pointMetas.size()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "点表在生成期间发生变化，请重试");
  }
  for (const auto& meta : pointMetas) {
    const auto currentPoint = it->second.pointTable.FindByTag(meta.tag);
    if (!currentPoint || currentPoint->ioa != meta.point.ioa || currentPoint->type != meta.point.type
        || currentPoint->businessType != meta.point.businessType) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "点表在生成期间发生变化，请重试");
    }
  }
  it->second.simulationValues = std::move(generatedValues);
  // 先写入快照，再由订阅线程按 Tag 屏蔽对应真实值，避免模拟快照尚未就绪。
  const char* boolModeLabel = "随机";
  switch (boolMode) {
  case IEC104Proto::SIMULATION_BOOL_MODE_ALL_FALSE:
    boolModeLabel = "全假";
    break;
  case IEC104Proto::SIMULATION_BOOL_MODE_ALL_TRUE:
    boolModeLabel = "全真";
    break;
  case IEC104Proto::SIMULATION_BOOL_MODE_INVERT_CURRENT:
    boolModeLabel = "取反";
    break;
  case IEC104Proto::SIMULATION_BOOL_MODE_RANDOM:
  default:
    break;
  }
  LOG_INFO("IEC104 已生成固定模拟值: conn_name={}, 遥测模式={}, 遥信模式={}, 遥测点数={}, 总点数={}, 已排除非模拟业务点数={}",
           connName, incremental ? "递增" : "随机", boolModeLabel, floatIndex, it->second.simulationValues.size(),
           configuredPointCount - it->second.simulationValues.size());
  return fillSimulationSnapshotLocked(it->second, out);
}

grpc::Status LinkManager::GetSimulationSnapshot(
    const std::string& connName, IEC104Proto::SimulationSnapshot* out) const {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  if (!isSlaveStation(it->second.config)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模拟值仅支持 IEC104 从站链路");
  }
  return fillSimulationSnapshotLocked(it->second, out);
}

grpc::Status LinkManager::ApplySimulationValues(const std::string& connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  auto& link = it->second;
  if (!isSlaveStation(link.config)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "仅允许 IEC104 从站应用模拟值");
  }
  if (link.simulationValues.empty()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "当前没有模拟值，请先生成");
  }
  if (link.state != IEC104Proto::LINK_STATE_RUNNING || !link.transport) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路未运行");
  }
  for (const auto& tag : link.pointTable.Tags()) {
    auto simIt = link.simulationValues.find(tag);
    auto point = link.pointTable.FindByTag(tag);
    if (simIt == link.simulationValues.end() || !point) {
      continue;
    }
    PointValue pv;
    pv.ioa = point->ioa;
    pv.type = point->type;
    pv.quality = static_cast<uint8_t>(simIt->second.quality());
    pv.tsMs = simIt->second.ts_ms();
    if (point->type == IEC104Proto::POINT_TYPE_FLOAT && simIt->second.has_double_value()) {
      if (!reverseScale(simIt->second.double_value(), point->scale, point->offset, &pv.doubleValue)) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "模拟浮点值无法按点表缩放");
      }
    } else if (point->type == IEC104Proto::POINT_TYPE_SINGLE && simIt->second.has_bool_value()) {
      pv.boolValue = simIt->second.bool_value();
    } else {
      continue;
    }
    link.transport->SendPointValue(pv, kCotSpontaneous);
  }
  LOG_INFO("IEC104 已发送固定模拟值: conn_name={}, 点数={}", connName, link.simulationValues.size());
  return grpc::Status::OK;
}

grpc::Status LinkManager::ClearSimulationValues(const std::string& connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  if (!isSlaveStation(it->second.config)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "模拟值仅支持 IEC104 从站链路");
  }
  it->second.simulationValues.clear();
  LOG_INFO("IEC104 已清除模拟值并恢复 DataCenter 数据路径: conn_name={}", connName);
  return grpc::Status::OK;
}

}  // namespace IEC104
