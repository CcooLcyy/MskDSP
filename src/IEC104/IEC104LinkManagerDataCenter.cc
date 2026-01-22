#include "IEC104LinkManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Logger.h"
#include "IEC104LibInfo.h"

namespace IEC104 {
namespace {
constexpr uint8_t kIec104QualityGood = 0x00;
constexpr uint8_t kIec104QualityInvalid = 0x80;

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

  link->dcSubscribeThread = std::jthread([this, connName, ctx, connId, tags, metaByTag, transport](std::stop_token st) {
    ModuleManager::LogModuleScope moduleScope(IEC104LibInfo.LIB_NAME);
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

  link->dcTimeSyncThread = std::jthread([this, connName, ctx, connId, tags, transport](std::stop_token st) {
    ModuleManager::LogModuleScope moduleScope(IEC104LibInfo.LIB_NAME);
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

  link->dcCommandThread = std::jthread([this, connName, ctx, connId, tags, metaByTag, transport](std::stop_token st) {
    ModuleManager::LogModuleScope moduleScope(IEC104LibInfo.LIB_NAME);
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

grpc::Status LinkManager::handleCommandValue(const std::string& connName, const CommandValue& cv) {
  uint32_t connId = 0;
  std::string tag;
  IEC104Proto::PointType type = IEC104Proto::POINT_TYPE_UNSPECIFIED;
  double scale = 1.0;
  double offset = 0.0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    if (!isSlaveStation(it->second.config)) {
      LOG_INFO("IEC104 非从站收到命令，忽略发布: conn_name={}", connName);
      return grpc::Status::OK;
    }
    connId = it->second.connId;
    auto p = it->second.pointTable.FindByIoa(cv.ioa);
    if (!p) {
      return grpc::Status::OK;
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
    return grpc::Status::OK;
  }

  if (type == IEC104Proto::POINT_TYPE_FLOAT) {
    const double engValue = applyScale(cv.doubleValue, scale, offset);
    auto st = dataCenter_.PublishDouble(connId, tag, engValue, DataCenterProto::QUALITY_GOOD, 0);
    if (!st.ok()) {
      LOG_WARNING("IEC104 发布设点失败: conn_name={}, tag={}, 错误={}", connName, tag, st.error_message());
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastError = st.error_message();
      }
    } else {
      LOG_INFO("IEC104 已发布设点: conn_name={}, tag={}, value={}", connName, tag, engValue);
    }
    return st;
  }

  if (type == IEC104Proto::POINT_TYPE_SINGLE) {
    auto st = dataCenter_.PublishBool(connId, tag, cv.boolValue, DataCenterProto::QUALITY_GOOD, 0);
    if (!st.ok()) {
      LOG_WARNING("IEC104 发布遥控失败: conn_name={}, tag={}, 错误={}", connName, tag, st.error_message());
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastError = st.error_message();
      }
    } else {
      LOG_INFO("IEC104 已发布遥控: conn_name={}, tag={}, value={}", connName, tag, cv.boolValue);
    }
    return st;
  }

  LOG_WARNING("IEC104 命令点类型不匹配: conn_name={}, tag={}, type={}", connName, tag, static_cast<int>(type));
  return grpc::Status::OK;
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
    return {};
  }

  std::vector<PointValue> out;
  out.reserve(static_cast<size_t>(resp.updates_size()));
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
      out.emplace_back(std::move(mv));
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
      out.emplace_back(std::move(pv));
    }
  }
  return out;
}

}  // namespace IEC104
