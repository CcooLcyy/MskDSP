#include "DataCenterCore.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

namespace DataCenter {
int64_t DataCenterCore::nowMs() {
  auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
  return now.time_since_epoch().count();
}

grpc::Status DataCenterCore::Publish(const DataCenterProto::PublishRequest &request, std::vector<DataCenterProto::PointUpdate> *outUpdates) {
  if (outUpdates == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "outUpdates 为空");
  }
  auto status = validateEndpoint(request.conn_id(), request.tag());
  if (!status.ok()) {
    return status;
  }
  if (request.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "value 不能为空");
  }

  int64_t tsMs = request.ts_ms();
  if (tsMs <= 0) {
    tsMs = nowMs();
  }

  outUpdates->clear();
  auto connIt = connections_.find(request.conn_id());
  if (connIt == connections_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "conn_id 未在连接注册表中找到，无法匹配稳定路由");
  }
  StableEndpointKey src{connIt->second.module_name(), connIt->second.conn_name(), request.tag()};
  auto it = routes_.find(src);
  if (it != routes_.end()) {
    for (const auto &dst : it->second) {
      if (!tryResolveConnId(dst, nullptr)) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "目的连接未在连接注册表中找到，无法生成路由更新");
      }
    }
  }

  if (sourceUpdateSequence_ == std::numeric_limits<uint64_t>::max()) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "源端最新值 sequence 已耗尽");
  }
  DataCenterProto::SourcePointUpdate sourceUpdate;
  sourceUpdate.set_conn_id(request.conn_id());
  sourceUpdate.set_tag(request.tag());
  sourceUpdate.mutable_value()->CopyFrom(request.value());
  sourceUpdate.set_ts_ms(tsMs);
  sourceUpdate.set_quality(request.quality());
  sourceUpdate.set_sequence(++sourceUpdateSequence_);
  latestBySrc_[EndpointKey{request.conn_id(), request.tag()}] = std::move(sourceUpdate);

  if (it == routes_.end()) {
    return grpc::Status::OK;
  }

  outUpdates->reserve(it->second.size());
  for (const auto &dst : it->second) {
    uint32_t dstConnId = 0;
    (void)tryResolveConnId(dst, &dstConnId);
    DataCenterProto::PointUpdate update;
    update.set_src_conn_id(request.conn_id());
    update.set_src_tag(request.tag());
    update.set_dst_conn_id(dstConnId);
    update.set_dst_tag(dst.tag);
    update.mutable_value()->CopyFrom(request.value());
    update.set_ts_ms(tsMs);
    update.set_quality(request.quality());

    latestByDst_[EndpointKey{dstConnId, dst.tag}] = update;
    outUpdates->emplace_back(std::move(update));
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::BatchPublish(const DataCenterProto::BatchPublishRequest &request, std::vector<DataCenterProto::PointUpdate> *outUpdates) {
  if (outUpdates == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "outUpdates 为空");
  }
  outUpdates->clear();

  for (const auto &point : request.points()) {
    auto status = validateEndpoint(point.conn_id(), point.tag());
    if (!status.ok()) {
      return status;
    }
    if (point.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "value 不能为空");
    }
  }

  size_t estimatedUpdates = 0;
  for (const auto &point : request.points()) {
    auto connIt = connections_.find(point.conn_id());
    if (connIt == connections_.end()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "conn_id 未在连接注册表中找到，无法匹配稳定路由");
    }
    StableEndpointKey src{connIt->second.module_name(), connIt->second.conn_name(), point.tag()};
    auto it = routes_.find(src);
    if (it == routes_.end()) {
      continue;
    }
    for (const auto &dst : it->second) {
      if (!tryResolveConnId(dst, nullptr)) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "目的连接未在连接注册表中找到，无法生成路由更新");
      }
    }
    estimatedUpdates += it->second.size();
  }

  const auto pointsCount = static_cast<uint64_t>(request.points_size());
  if (pointsCount > std::numeric_limits<uint64_t>::max() - sourceUpdateSequence_) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "源端最新值 sequence 已耗尽");
  }

  std::vector<DataCenterProto::SourcePointUpdate> sourceUpdates;
  sourceUpdates.reserve(static_cast<size_t>(request.points_size()));
  outUpdates->reserve(estimatedUpdates);

  uint64_t nextSourceSequence = sourceUpdateSequence_;

  for (const auto &point : request.points()) {
    auto connIt = connections_.find(point.conn_id());
    if (connIt == connections_.end()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "conn_id 未在连接注册表中找到，无法匹配稳定路由");
    }
    int64_t tsMs = point.ts_ms();
    if (tsMs <= 0) {
      tsMs = nowMs();
    }

    DataCenterProto::SourcePointUpdate sourceUpdate;
    sourceUpdate.set_conn_id(point.conn_id());
    sourceUpdate.set_tag(point.tag());
    sourceUpdate.mutable_value()->CopyFrom(point.value());
    sourceUpdate.set_ts_ms(tsMs);
    sourceUpdate.set_quality(point.quality());
    sourceUpdate.set_sequence(++nextSourceSequence);
    sourceUpdates.emplace_back(std::move(sourceUpdate));

    StableEndpointKey src{connIt->second.module_name(), connIt->second.conn_name(), point.tag()};
    auto it = routes_.find(src);
    if (it == routes_.end()) {
      continue;
    }

    for (const auto &dst : it->second) {
      uint32_t dstConnId = 0;
      if (!tryResolveConnId(dst, &dstConnId)) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "目的连接未在连接注册表中找到，无法生成路由更新");
      }
      DataCenterProto::PointUpdate update;
      update.set_src_conn_id(point.conn_id());
      update.set_src_tag(point.tag());
      update.set_dst_conn_id(dstConnId);
      update.set_dst_tag(dst.tag);
      update.mutable_value()->CopyFrom(point.value());
      update.set_ts_ms(tsMs);
      update.set_quality(point.quality());
      outUpdates->emplace_back(std::move(update));
    }
  }

  for (const auto &sourceUpdate : sourceUpdates) {
    latestBySrc_[EndpointKey{sourceUpdate.conn_id(), sourceUpdate.tag()}] = sourceUpdate;
  }
  sourceUpdateSequence_ = nextSourceSequence;

  for (const auto &update : *outUpdates) {
    EndpointKey dst{update.dst_conn_id(), update.dst_tag()};
    latestByDst_[std::move(dst)] = update;
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::ResolveCommandRoute(
    const DataCenterProto::ExecuteCommandRequest &request,
    DataCenterProto::ExecuteCommandResponse *out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  out->Clear();
  if (!request.has_src()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "src 不能为空");
  }
  if (request.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "value 不能为空");
  }

  StableEndpointKey src;
  uint32_t srcConnId = 0;
  auto status = resolveEndpoint(request.src(), &src, &srcConnId);
  if (!status.ok()) {
    return status;
  }

  auto it = routes_.find(src);
  if (it == routes_.end() || it->second.empty()) {
    out->set_status(DataCenterProto::COMMAND_NO_ROUTE);
    out->set_reason("命令源端点未匹配到路由");
    return grpc::Status::OK;
  }
  std::vector<const StableEndpointKey *> businessDsts;
  businessDsts.reserve(it->second.size());
  for (const auto &dst : it->second) {
    businessDsts.emplace_back(&dst);
  }
  if (businessDsts.empty()) {
    out->set_status(DataCenterProto::COMMAND_NO_ROUTE);
    out->set_reason("命令源端点未匹配到业务路由");
    return grpc::Status::OK;
  }
  if (businessDsts.size() != 1) {
    out->set_status(DataCenterProto::COMMAND_AMBIGUOUS_ROUTE);
    out->set_reason("命令源端点匹配到多条业务路由，无法形成单一协议确认");
    return grpc::Status::OK;
  }

  const auto &dst = *businessDsts.front();
  uint32_t dstConnId = 0;
  if (!tryResolveConnId(dst, &dstConnId)) {
    out->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
    out->set_reason("目的连接未在连接注册表中找到");
    return grpc::Status::OK;
  }

  auto *respDst = out->mutable_dst();
  respDst->set_module_name(dst.moduleName);
  respDst->set_conn_name(dst.connName);
  respDst->set_conn_id(dstConnId);
  respDst->set_tag(dst.tag);
  out->set_status(DataCenterProto::COMMAND_STATUS_UNSPECIFIED);

  switch (request.value().kind_case()) {
    case DataCenterProto::PointValue::kDoubleValue:
      out->set_requested_value(request.value().double_value());
      break;
    case DataCenterProto::PointValue::kIntValue:
      out->set_requested_value(static_cast<double>(request.value().int_value()));
      break;
    case DataCenterProto::PointValue::kBoolValue:
      out->set_requested_value(request.value().bool_value() ? 1.0 : 0.0);
      break;
    case DataCenterProto::PointValue::kStringValue:
    case DataCenterProto::PointValue::kBytesValue:
    case DataCenterProto::PointValue::KIND_NOT_SET:
    default:
      break;
  }
  (void)srcConnId;
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::StoreAcceptedCommand(
    const DataCenterProto::ExecuteCommandRequest &request,
    const DataCenterProto::Endpoint &dst) {
  if (!request.has_src()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "src 不能为空");
  }
  if (request.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "value 不能为空");
  }
  if (dst.conn_id() == 0 || dst.tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "dst 不能为空");
  }

  StableEndpointKey src;
  uint32_t srcConnId = 0;
  auto status = resolveEndpoint(request.src(), &src, &srcConnId);
  if (!status.ok()) {
    return status;
  }
  int64_t tsMs = request.ts_ms();
  if (tsMs <= 0) {
    tsMs = nowMs();
  }

  DataCenterProto::PointUpdate update;
  update.set_src_conn_id(srcConnId);
  update.set_src_tag(src.tag);
  update.set_dst_conn_id(dst.conn_id());
  update.set_dst_tag(dst.tag());
  update.mutable_value()->CopyFrom(request.value());
  update.set_ts_ms(tsMs);
  update.set_quality(request.quality());
  latestByDst_[EndpointKey{dst.conn_id(), dst.tag()}] = std::move(update);
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::GetLatest(const DataCenterProto::GetLatestRequest &request, DataCenterProto::GetLatestResponse *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (request.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  for (const auto &tag : request.tags()) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags 包含空字符串");
    }
  }

  std::unordered_set<std::string> filter;
  if (request.tags_size() > 0) {
    filter.reserve(static_cast<size_t>(request.tags_size()));
    for (const auto &tag : request.tags()) {
      filter.emplace(tag);
    }
  }

  std::vector<DataCenterProto::PointUpdate> tmp;
  for (const auto &[dst, update] : latestByDst_) {
    if (dst.connId != request.conn_id()) {
      continue;
    }
    if (!filter.empty() && !filter.contains(dst.tag)) {
      continue;
    }
    tmp.emplace_back(update);
  }

  std::sort(tmp.begin(), tmp.end(), [](const auto &a, const auto &b) { return a.dst_tag() < b.dst_tag(); });

  out->Clear();
  for (const auto &update : tmp) {
    *out->add_updates() = update;
  }
  return grpc::Status::OK;
}

grpc::Status DataCenterCore::GetSourceLatest(
    const DataCenterProto::GetSourceLatestRequest &request,
    DataCenterProto::GetSourceLatestResponse *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (request.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
  }
  for (const auto &tag : request.tags()) {
    if (tag.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags 包含空字符串");
    }
  }
  if (!connections_.contains(request.conn_id())) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "conn_id 未在连接注册表中找到");
  }

  std::unordered_set<std::string> filter;
  if (request.tags_size() > 0) {
    filter.reserve(static_cast<size_t>(request.tags_size()));
    for (const auto &tag : request.tags()) {
      filter.emplace(tag);
    }
  }

  std::vector<DataCenterProto::SourcePointUpdate> tmp;
  for (const auto &[src, update] : latestBySrc_) {
    if (src.connId != request.conn_id()) {
      continue;
    }
    if (!filter.empty() && !filter.contains(src.tag)) {
      continue;
    }
    tmp.emplace_back(update);
  }
  std::sort(tmp.begin(), tmp.end(), [](const auto &a, const auto &b) { return a.tag() < b.tag(); });

  out->Clear();
  for (const auto &update : tmp) {
    *out->add_updates() = update;
  }
  return grpc::Status::OK;
}
}  // namespace DataCenter
