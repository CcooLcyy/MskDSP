#include "IEC104LinkManager.h"

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
  if (link == nullptr || link->config.role() != IEC104Proto::ROLE_SERVER || !link->transport) {
    return;
  }
  stopDataCenterSubscribeLocked(link);

  auto tags = link->pointTable.Tags();
  struct PointMeta {
    uint32_t ioa = 0;
    double scale = 1.0;
    double offset = 0.0;
    double deadband = 0.0;
  };
  std::unordered_map<std::string, PointMeta> metaByTag;
  metaByTag.reserve(tags.size());
  for (const auto& tag : tags) {
    auto p = link->pointTable.FindByTag(tag);
    if (p) {
      metaByTag.emplace(tag, PointMeta{p->ioa, p->scale, p->offset, p->deadband});
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
      double value = 0;
      if (!pointValueToDouble(update.value(), &value)) {
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
      uint8_t qds = toIec104Quality(update.quality());
      transport->SendMeasuredValue(it->second.ioa, rawValue, qds, kCotSpontaneous);
      lastSentByTag[update.dst_tag()] = value;
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

grpc::Status LinkManager::handleClientMeasuredValue(const std::string& connName, const MeasuredValue& mv) {
  uint32_t connId = 0;
  std::string tag;
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
    auto p = it->second.pointTable.FindByIoa(mv.ioa);
    if (!p) {
      return grpc::Status::OK;
    }
    tag = p->tag;
    scale = p->scale;
    offset = p->offset;
    deadband = p->deadband;
    auto lastIt = it->second.lastReportedByTag.find(tag);
    if (lastIt != it->second.lastReportedByTag.end()) {
      last = lastIt->second;
    }
  }

  auto quality = toDataCenterQuality(mv.quality);
  const double engValue = applyScale(mv.value, scale, offset);
  if (!shouldReport(engValue, deadband, last)) {
    LOG_DEBUG("IEC104 死区过滤上报: conn_name={}, tag={}, value={}, last={}, 死区={}",
              connName,
              tag,
              engValue,
              last.value(),
              deadband);
    return grpc::Status::OK;
  }
  auto st = dataCenter_.PublishDouble(connId, tag, engValue, quality, 0);
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

std::vector<MeasuredValue> LinkManager::buildInterrogationSnapshot(const std::string& connName) {
  uint32_t connId = 0;
  struct PointMeta {
    uint32_t ioa = 0;
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
        metaByTag.emplace(tag, PointMeta{p->ioa, p->scale, p->offset});
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

  std::vector<MeasuredValue> out;
  out.reserve(static_cast<size_t>(resp.updates_size()));
  for (const auto& update : resp.updates()) {
    auto it = metaByTag.find(update.dst_tag());
    if (it == metaByTag.end()) {
      continue;
    }
    double value = 0;
    if (!pointValueToDouble(update.value(), &value)) {
      continue;
    }
    double rawValue = 0;
    if (!reverseScale(value, it->second.scale, it->second.offset, &rawValue)) {
      LOG_WARNING("IEC104 总召点值反向缩放失败: conn_name={}, tag={}, value={}", connName, update.dst_tag(), value);
      continue;
    }
    MeasuredValue mv;
    mv.ioa = it->second.ioa;
    mv.value = rawValue;
    mv.quality = toIec104Quality(update.quality());
    out.emplace_back(std::move(mv));
  }
  return out;
}

}  // namespace IEC104
