#include "IEC104LinkManager.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <time.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Logger.h"
#include "IEC104LibInfo.h"
#include "IEC104ReportPolicy.hpp"
#include "IEC104SoeStore.h"
#include "ThreadUtil.hpp"
#include "mskdsp/Decimal20.hpp"

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

bool applyScale(double raw,
                const mskdsp::numeric::Decimal20& scale,
                const mskdsp::numeric::Decimal20& offset,
                mskdsp::numeric::Decimal20* out) {
  if (out == nullptr) {
    return false;
  }
  // IEC104 遥测和遥调使用 short float，先按实际协议精度进入业务计算。
  auto rawDecimal =
      mskdsp::numeric::Decimal20::FromFloat(static_cast<float>(raw));
  if (!rawDecimal) {
    return false;
  }
  auto engineering =
      mskdsp::numeric::ApplyEngineering(*rawDecimal, scale, offset);
  if (!engineering) {
    return false;
  }
  *out = *engineering;
  return true;
}

bool reverseScale(const mskdsp::numeric::Decimal20& engineering,
                  const mskdsp::numeric::Decimal20& scale,
                  const mskdsp::numeric::Decimal20& offset,
                  double* out) {
  if (out == nullptr) {
    return false;
  }
  auto raw =
      mskdsp::numeric::ReverseEngineering(engineering, scale, offset);
  if (!raw) {
    return false;
  }
  // 在 ASDU 边界完成 binary32 范围检查和量化，线格式仍保持 short float。
  auto boundary = raw->ToFloat();
  if (!boundary) {
    return false;
  }
  *out = static_cast<double>(*boundary);
  return true;
}

bool reverseScale(double engineering,
                  const mskdsp::numeric::Decimal20& scale,
                  const mskdsp::numeric::Decimal20& offset,
                  double* out) {
  auto decimal = mskdsp::numeric::Decimal20::FromDouble(engineering);
  return decimal && reverseScale(*decimal, scale, offset, out);
}

bool shouldReport(const mskdsp::numeric::Decimal20& value,
                  const mskdsp::numeric::Decimal20& deadband,
                  const std::optional<mskdsp::numeric::Decimal20>& last) {
  if (deadband <= mskdsp::numeric::Decimal20{} || !last.has_value()) {
    return true;
  }
  return mskdsp::numeric::ShouldReport(value, *last, deadband);
}

bool pointValueToDecimal(const DataCenterProto::PointValue& v,
                         mskdsp::numeric::Decimal20* out) {
  if (out == nullptr) {
    return false;
  }
  switch (v.kind_case()) {
  case DataCenterProto::PointValue::kDoubleValue: {
    auto value = mskdsp::numeric::Decimal20::FromDouble(v.double_value());
    if (value) {
      *out = *value;
    }
    return value.has_value();
  }
  case DataCenterProto::PointValue::kIntValue: {
    auto value = mskdsp::numeric::Decimal20::FromInt64(v.int_value());
    if (value) {
      *out = *value;
    }
    return value.has_value();
  }
  case DataCenterProto::PointValue::kBoolValue: {
    auto value =
        mskdsp::numeric::Decimal20::FromInt64(v.bool_value() ? 1 : 0);
    if (value) {
      *out = *value;
    }
    return value.has_value();
  }
  case DataCenterProto::PointValue::kDecimalValue: {
    auto value = mskdsp::numeric::Decimal20::Parse(v.decimal_value());
    if (value) {
      *out = *value;
    }
    return value.has_value();
  }
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
  case DataCenterProto::PointValue::kDecimalValue: {
    auto decimal = mskdsp::numeric::Decimal20::Parse(v.decimal_value());
    if (!decimal) {
      return false;
    }
    *out = !decimal->IsZero();
    return true;
  }
  default:
    return false;
  }
}

bool pointValueToDoubleControl(const DataCenterProto::PointValue& value, uint8_t* out) {
  if (out == nullptr) {
    return false;
  }
  switch (value.kind_case()) {
  case DataCenterProto::PointValue::kIntValue:
    if (value.int_value() != 1 && value.int_value() != 2) {
      return false;
    }
    *out = static_cast<uint8_t>(value.int_value());
    return true;
  case DataCenterProto::PointValue::kBoolValue:
    *out = value.bool_value() ? 2 : 1;
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

bool LinkManager::storeAndSendSoe(const std::string& connName,
                                  const PointValue& pv,
                                  TcpLink* transport) {
  if (transport == nullptr) {
    LOG_ERROR("IEC104 SOE 上送失败: conn_name={}, ioa={}, 原因=传输对象为空", connName, pv.ioa);
    return false;
  }
  if (!soeStore_) {
    LOG_WARNING("IEC104 未启用 SOE 持久化，按普通点值上送: conn_name={}, ioa={}", connName, pv.ioa);
    transport->SendPointValue(pv, kCotSpontaneous);
    return true;
  }

  PointValue persistedValue = pv;
  if (persistedValue.tsMs <= 0) {
    persistedValue.tsMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    LOG_WARNING("IEC104 SOE 缺少有效时标，使用当前时间: conn_name={}, ioa={}, ts_ms={}",
                connName,
                persistedValue.ioa,
                persistedValue.tsMs);
  }

  SoeAppendResult result;
  auto status = soeStore_->Append(connName,
                                  persistedValue.ioa,
                                  persistedValue.boolValue,
                                  persistedValue.tsMs,
                                  persistedValue.quality,
                                  &result);
  if (!status.ok()) {
    LOG_ERROR("IEC104 SOE 落盘失败，不执行上送: conn_name={}, ioa={}, ts_ms={}, 原因={}",
              connName,
              persistedValue.ioa,
              persistedValue.tsMs,
              status.error_message());
    return false;
  }

  SoeEvent event;
  event.eventSequence = result.record.eventSequence;
  event.value = persistedValue;
  transport->SendSoe(event);
  LOG_DEBUG("IEC104 SOE 已落盘并提交发送: conn_name={}, event_seq={}, ioa={}, 状态={}, ts_ms={}, 品质={}",
            connName,
            event.eventSequence,
            persistedValue.ioa,
            persistedValue.boolValue ? "合" : "分",
            persistedValue.tsMs,
            persistedValue.quality);
  return true;
}

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
    mskdsp::numeric::Decimal20 scale =
        mskdsp::numeric::Decimal20::FromInt64(1).value();
    mskdsp::numeric::Decimal20 offset;
    mskdsp::numeric::Decimal20 deadband;
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

    std::unordered_map<std::string, detail::LastReportedTelemetry> lastSentByTag;
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
        mskdsp::numeric::Decimal20 value;
        if (!pointValueToDecimal(update.value(), &value)) {
          LOG_DEBUG("IEC104 遥测点值类型不匹配: conn_name={}, tag={}", connName, update.dst_tag());
          continue;
        }
        std::optional<detail::LastReportedTelemetry> last;
        auto lastIt = lastSentByTag.find(update.dst_tag());
        if (lastIt != lastSentByTag.end()) {
          last = lastIt->second;
        }
        if (!detail::ShouldReportTelemetry(value, it->second.deadband, update.quality(), update.ts_ms(), last)) {
          LOG_DEBUG("IEC104 死区过滤上送: conn_name={}, tag={}, value={}, last={}, 品质={}, ts_ms={}, 死区={}",
                    connName,
                    update.dst_tag(),
                    value.ToString(),
                    last->value.ToString(),
                    static_cast<int>(update.quality()),
                    update.ts_ms(),
                    it->second.deadband.ToString());
          continue;
        }
        double rawValue = 0;
        if (!reverseScale(value, it->second.scale, it->second.offset, &rawValue)) {
          LOG_WARNING("IEC104 点值反向缩放失败: conn_name={}, tag={}, value={}",
                      connName, update.dst_tag(), value.ToString());
          continue;
        }
        PointValue pv;
        pv.ioa = it->second.ioa;
        pv.type = IEC104Proto::POINT_TYPE_FLOAT;
        pv.doubleValue = rawValue;
        pv.quality = toIec104Quality(update.quality());
        pv.tsMs = update.ts_ms();
        transport->SendPointValue(pv, kCotSpontaneous);
        lastSentByTag[update.dst_tag()] = detail::LastReportedTelemetry{
            .value = value,
            .quality = update.quality(),
            .tsMs = update.ts_ms(),
        };
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
        (void)storeAndSendSoe(connName, pv, transport);
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
        case DataCenterProto::PointValue::kDecimalValue: {
          auto decimal = mskdsp::numeric::Decimal20::Parse(
              update.value().decimal_value());
          if (decimal) {
            auto integer = decimal->ToInteger<int64_t>(
                mskdsp::numeric::RoundingMode::kTowardZero);
            if (integer) {
              tsMs = *integer;
            }
          }
          break;
        }
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

  struct PointMeta {
    uint32_t ioa = 0;
    IEC104Proto::PointType type = IEC104Proto::POINT_TYPE_UNSPECIFIED;
    IEC104Proto::PointBusinessType businessType = IEC104Proto::POINT_BUSINESS_TYPE_UNSPECIFIED;
    IEC104Proto::RemoteControlType remoteControlType = IEC104Proto::REMOTE_CONTROL_TYPE_SINGLE;
    IEC104Proto::CommandExecutionMode commandExecutionMode =
        IEC104Proto::COMMAND_EXECUTION_MODE_SELECT_EXECUTE;
    mskdsp::numeric::Decimal20 scale =
        mskdsp::numeric::Decimal20::FromInt64(1).value();
    mskdsp::numeric::Decimal20 offset;
  };
  std::vector<std::string> tags;
  std::unordered_map<std::string, PointMeta> metaByTag;
  const auto pointTags = link->pointTable.Tags();
  metaByTag.reserve(pointTags.size());
  tags.reserve(pointTags.size());
  for (const auto& tag : pointTags) {
    auto p = link->pointTable.FindByTag(tag);
    if (!p || (p->businessType != IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_CONTROL
               && p->businessType != IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_ADJUST)) {
      continue;
    }
    tags.emplace_back(tag);
    metaByTag.emplace(tag, PointMeta{p->ioa,
                                     p->type,
                                     p->businessType,
                                     p->remoteControlType,
                                     p->commandExecutionMode,
                                     p->scale,
                                     p->offset});
  }
  if (tags.empty()) {
    LOG_INFO("IEC104 命令订阅无遥控或遥调点: conn_name={}", connName);
    return;
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
      if (it->second.businessType == IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_ADJUST) {
        if (it->second.type != IEC104Proto::POINT_TYPE_FLOAT) {
          LOG_WARNING("IEC104 遥调点协议类型不是浮点: conn_name={}, tag={}", connName, update.dst_tag());
          continue;
        }
        mskdsp::numeric::Decimal20 value;
        if (!pointValueToDecimal(update.value(), &value)) {
          LOG_DEBUG("IEC104 设点点值类型不匹配: conn_name={}, tag={}", connName, update.dst_tag());
          continue;
        }
        double rawValue = 0;
        if (!reverseScale(value, it->second.scale, it->second.offset, &rawValue)) {
          LOG_WARNING("IEC104 设点反向缩放失败: conn_name={}, tag={}, value={}",
                      connName, update.dst_tag(), value.ToString());
          continue;
        }
        LOG_INFO("IEC104 触发设点命令: conn_name={}, tag={}, ioa={}, value={}",
                 connName, update.dst_tag(), it->second.ioa, value.ToString());
        transport->SendSetpointCommand(it->second.ioa, rawValue);
      } else if (it->second.businessType == IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_CONTROL) {
        if (it->second.type != IEC104Proto::POINT_TYPE_SINGLE) {
          LOG_WARNING("IEC104 遥控点协议类型不是单点: conn_name={}, tag={}", connName, update.dst_tag());
          continue;
        }
        uint8_t value = 0;
        if (it->second.remoteControlType == IEC104Proto::REMOTE_CONTROL_TYPE_DOUBLE) {
          if (!pointValueToDoubleControl(update.value(), &value)) {
            LOG_WARNING("IEC104 双点遥控值非法，要求 int64 1/2 或 BOOL: conn_name={}, tag={}",
                        connName, update.dst_tag());
            continue;
          }
        } else {
          bool boolValue = false;
          if (!pointValueToBool(update.value(), &boolValue)) {
            LOG_DEBUG("IEC104 单点遥控值类型不匹配: conn_name={}, tag={}", connName, update.dst_tag());
            continue;
          }
          value = boolValue ? 1 : 0;
        }
        LOG_INFO("IEC104 触发遥控命令: conn_name={}, tag={}, ioa={}, type={}, value={}, mode={}",
                 connName, update.dst_tag(), it->second.ioa,
                 static_cast<int>(it->second.remoteControlType), value,
                 static_cast<int>(it->second.commandExecutionMode));
        transport->SendRemoteControl(it->second.ioa,
                                     it->second.remoteControlType,
                                     value,
                                     it->second.commandExecutionMode);
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
  mskdsp::numeric::Decimal20 scale =
      mskdsp::numeric::Decimal20::FromInt64(1).value();
  mskdsp::numeric::Decimal20 offset;
  mskdsp::numeric::Decimal20 deadband;
  std::optional<mskdsp::numeric::Decimal20> last;
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
    mskdsp::numeric::Decimal20 engValue;
    if (!applyScale(pv.doubleValue, scale, offset, &engValue)) {
      LOG_WARNING("IEC104 工程量换算失败: conn_name={}, tag={}, raw={}, scale={}, offset={}",
                  connName, tag, pv.doubleValue, scale.ToString(), offset.ToString());
      return grpc::Status(grpc::StatusCode::OUT_OF_RANGE,
                          "IEC104 工程量换算结果超出 Decimal20 范围");
    }
    if (!shouldReport(engValue, deadband, last)) {
      LOG_DEBUG("IEC104 死区过滤上报: conn_name={}, tag={}, value={}, last={}, 死区={}",
                connName,
                tag,
                engValue.ToString(),
                last->ToString(),
                deadband.ToString());
      return grpc::Status::OK;
    }
    auto st = dataCenter_.PublishDecimal(
        connId, tag, engValue.ToFixedString(), quality, pv.tsMs);
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
  IEC104Proto::PointBusinessType businessType = IEC104Proto::POINT_BUSINESS_TYPE_UNSPECIFIED;
  IEC104Proto::RemoteControlType remoteControlType = IEC104Proto::REMOTE_CONTROL_TYPE_SINGLE;
  mskdsp::numeric::Decimal20 scale =
      mskdsp::numeric::Decimal20::FromInt64(1).value();
  mskdsp::numeric::Decimal20 offset;
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
    businessType = p->businessType;
    remoteControlType = p->remoteControlType;
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
    if (businessType != IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_ADJUST) {
      LOG_WARNING("IEC104 设点命令目标不是遥调点: conn_name={}, tag={}, business_type={}",
                  connName, tag, static_cast<int>(businessType));
      return rejectCommand("IEC104 设点命令目标不是遥调点");
    }
    mskdsp::numeric::Decimal20 engValue;
    if (!applyScale(cv.doubleValue, scale, offset, &engValue)) {
      LOG_WARNING("IEC104 设点工程量换算失败: conn_name={}, tag={}, raw={}, scale={}, offset={}",
                  connName, tag, cv.doubleValue, scale.ToString(), offset.ToString());
      return rejectCommand("IEC104 设点工程量换算失败");
    }
    req.mutable_value()->set_decimal_value(engValue.ToFixedString());
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
                  engValue.ToString(),
                  static_cast<int>(resp.status()),
                  static_cast<int>(resp.reject_code()),
                  reason);
      return rejectCommand(reason);
    }
    LOG_INFO("IEC104 设点同步执行成功: conn_name={}, tag={}, value={}",
             connName, tag, engValue.ToString());
    return CommandResult{};
  }

  if (type == IEC104Proto::POINT_TYPE_SINGLE) {
    if (businessType != IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_CONTROL) {
      LOG_WARNING("IEC104 遥控命令目标不是遥控点: conn_name={}, tag={}, business_type={}",
                  connName, tag, static_cast<int>(businessType));
      return rejectCommand("IEC104 遥控命令目标不是遥控点");
    }
    if (cv.remoteControlType != remoteControlType) {
      LOG_WARNING("IEC104 遥控命令类型不一致: conn_name={}, tag={}, 配置类型={}, 实际类型={}",
                  connName, tag, static_cast<int>(remoteControlType), static_cast<int>(cv.remoteControlType));
      return rejectCommand("IEC104 遥控命令类型不一致");
    }
    if (remoteControlType == IEC104Proto::REMOTE_CONTROL_TYPE_DOUBLE) {
      if (cv.controlValue != 1 && cv.controlValue != 2) {
        LOG_WARNING("IEC104 双点遥控状态非法: conn_name={}, tag={}, value={}",
                    connName, tag, cv.controlValue);
        return rejectCommand("IEC104 双点遥控状态非法");
      }
      req.mutable_value()->set_int_value(cv.controlValue);
    } else {
      req.mutable_value()->set_bool_value(cv.boolValue);
    }
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
                  remoteControlType == IEC104Proto::REMOTE_CONTROL_TYPE_DOUBLE
                      ? static_cast<int>(cv.controlValue)
                      : static_cast<int>(cv.boolValue),
                  static_cast<int>(resp.status()),
                  static_cast<int>(resp.reject_code()),
                  reason);
      return rejectCommand(reason);
    }
    LOG_INFO("IEC104 遥控同步执行成功: conn_name={}, tag={}, type={}, value={}",
             connName, tag, static_cast<int>(remoteControlType),
             remoteControlType == IEC104Proto::REMOTE_CONTROL_TYPE_DOUBLE
                 ? static_cast<int>(cv.controlValue)
                 : static_cast<int>(cv.boolValue));
    return CommandResult{};
  }

  LOG_WARNING("IEC104 命令点类型不匹配: conn_name={}, tag={}, type={}", connName, tag, static_cast<int>(type));
  return rejectCommand("IEC104 点类型不支持命令执行");
}

grpc::Status LinkManager::setSystemClock(int64_t tsMs) {
  timespec requested{};
  requested.tv_sec = static_cast<time_t>(tsMs / 1000);
  requested.tv_nsec = static_cast<long>((tsMs % 1000) * 1'000'000);
  if (::clock_settime(CLOCK_REALTIME, &requested) == 0) {
    return grpc::Status::OK;
  }

  const auto error = errno;
  return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                      std::format("errno={}: {}", error, std::strerror(error)));
}

grpc::Status LinkManager::handleTimeSyncCommand(const std::string& connName, int64_t tsMs) {
  if (tsMs <= 0) {
    LOG_WARNING("IEC104 对时时间戳无效: conn_name={}", connName);
    return grpc::Status::OK;
  }

  uint32_t connId = 0;
  std::string tag;
  bool shouldSetSystemTime = false;
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
    shouldSetSystemTime = it->second.config.set_system_time_on_sync();
  }

  if (shouldSetSystemTime) {
    auto status = systemTimeSetter_(tsMs);
    if (!status.ok()) {
      LOG_ERROR("IEC104 设置系统时间失败: conn_name={}, ts_ms={}, 原因={}",
                connName, tsMs, status.error_message());
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastError = status.error_message();
      }
      return status;
    }
    LOG_INFO("IEC104 已设置系统时间: conn_name={}, ts_ms={}", connName, tsMs);
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
  // DataCenter 事件发布属于旁路通知，不应影响 IEC104 对时本身的协议确认。
  // 只有系统时钟设置失败才返回 false，由传输层生成负确认。
  return grpc::Status::OK;
}

std::vector<PointValue> LinkManager::buildInterrogationSnapshot(const std::string& connName) {
  uint32_t connId = 0;
  struct PointMeta {
    uint32_t ioa = 0;
    IEC104Proto::PointType type = IEC104Proto::POINT_TYPE_UNSPECIFIED;
    mskdsp::numeric::Decimal20 scale =
        mskdsp::numeric::Decimal20::FromInt64(1).value();
    mskdsp::numeric::Decimal20 offset;
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
      mskdsp::numeric::Decimal20 value;
      if (!pointValueToDecimal(update.value(), &value)) {
        continue;
      }
      double rawValue = 0;
      if (!reverseScale(value, it->second.scale, it->second.offset, &rawValue)) {
        LOG_WARNING("IEC104 总召点值反向缩放失败: conn_name={}, tag={}, value={}",
                    connName, update.dst_tag(), value.ToString());
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
    if (pv.type == IEC104Proto::POINT_TYPE_SINGLE) {
      if (!storeAndSendSoe(connName, pv, link.transport.get())) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "模拟遥信 SOE 落盘失败");
      }
    } else {
      link.transport->SendPointValue(pv, kCotSpontaneous);
    }
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
