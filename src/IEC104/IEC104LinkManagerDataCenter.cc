#include "IEC104LinkManager.h"

#include <format>
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
  std::unordered_map<std::string, uint32_t> ioaByTag;
  ioaByTag.reserve(tags.size());
  for (const auto& tag : tags) {
    auto p = link->pointTable.FindByTag(tag);
    if (p) {
      ioaByTag.emplace(tag, p->ioa);
    }
  }

  auto* transport = link->transport.get();
  auto connId = link->connId;

  LOG_INFO("IEC104 启动 DataCenter 订阅: conn_name={}, conn_id={}, tags={}", connName, connId, tags.size());

  link->dcSubscribeContext = std::make_shared<grpc::ClientContext>();
  auto ctx = link->dcSubscribeContext;

  link->dcSubscribeThread = std::jthread([this, connName, ctx, connId, tags, ioaByTag, transport](std::stop_token st) {
    ModuleManager::LogModuleScope moduleScope(IEC104LibInfo.LIB_NAME);
    std::stop_callback cb(st, [&ctx]() { ctx->TryCancel(); });

    auto reader = dataCenter_.Subscribe(ctx.get(), connId, tags, false);
    if (!reader) {
      LOG_ERROR("IEC104 创建 DataCenter 订阅失败: conn_name={}, conn_id={}, tags={}", connName, connId, tags.size());
      return;
    }

    DataCenterProto::PointUpdate update;
    while (reader->Read(&update)) {
      auto it = ioaByTag.find(update.dst_tag());
      if (it == ioaByTag.end()) {
        continue;
      }
      double value = 0;
      if (!pointValueToDouble(update.value(), &value)) {
        continue;
      }
      uint8_t qds = toIec104Quality(update.quality());
      transport->SendMeasuredValue(it->second, value, qds, kCotSpontaneous);
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
  }

  auto quality = toDataCenterQuality(mv.quality);
  auto st = dataCenter_.PublishDouble(connId, tag, mv.value, quality, 0);
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

std::vector<MeasuredValue> LinkManager::buildInterrogationSnapshot(const std::string& connName) {
  uint32_t connId = 0;
  std::unordered_map<std::string, uint32_t> ioaByTag;
  std::vector<std::string> tags;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return {};
    }
    connId = it->second.connId;
    tags = it->second.pointTable.Tags();
    ioaByTag.reserve(tags.size());
    for (const auto& tag : tags) {
      auto p = it->second.pointTable.FindByTag(tag);
      if (p) {
        ioaByTag.emplace(tag, p->ioa);
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
    auto it = ioaByTag.find(update.dst_tag());
    if (it == ioaByTag.end()) {
      continue;
    }
    double value = 0;
    if (!pointValueToDouble(update.value(), &value)) {
      continue;
    }
    MeasuredValue mv;
    mv.ioa = it->second;
    mv.value = value;
    mv.quality = toIec104Quality(update.quality());
    out.emplace_back(std::move(mv));
  }
  return out;
}

}  // namespace IEC104
