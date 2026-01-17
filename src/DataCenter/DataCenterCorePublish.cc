#include "DataCenterCore.h"

#include <algorithm>
#include <chrono>
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
  EndpointKey src{request.conn_id(), request.tag()};
  auto it = routes_.find(src);
  if (it == routes_.end()) {
    return grpc::Status::OK;
  }

  outUpdates->reserve(it->second.size());
  for (const auto &dst : it->second) {
    DataCenterProto::PointUpdate update;
    update.set_src_conn_id(request.conn_id());
    update.set_src_tag(request.tag());
    update.set_dst_conn_id(dst.connId);
    update.set_dst_tag(dst.tag);
    update.mutable_value()->CopyFrom(request.value());
    update.set_ts_ms(tsMs);
    update.set_quality(request.quality());

    latestByDst_[dst] = update;
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
    EndpointKey src{point.conn_id(), point.tag()};
    auto it = routes_.find(src);
    if (it == routes_.end()) {
      continue;
    }
    estimatedUpdates += it->second.size();
  }
  outUpdates->reserve(estimatedUpdates);

  for (const auto &point : request.points()) {
    int64_t tsMs = point.ts_ms();
    if (tsMs <= 0) {
      tsMs = nowMs();
    }

    EndpointKey src{point.conn_id(), point.tag()};
    auto it = routes_.find(src);
    if (it == routes_.end()) {
      continue;
    }

    for (const auto &dst : it->second) {
      DataCenterProto::PointUpdate update;
      update.set_src_conn_id(point.conn_id());
      update.set_src_tag(point.tag());
      update.set_dst_conn_id(dst.connId);
      update.set_dst_tag(dst.tag);
      update.mutable_value()->CopyFrom(point.value());
      update.set_ts_ms(tsMs);
      update.set_quality(point.quality());
      outUpdates->emplace_back(std::move(update));
    }
  }

  for (const auto &update : *outUpdates) {
    EndpointKey dst{update.dst_conn_id(), update.dst_tag()};
    latestByDst_[std::move(dst)] = update;
  }
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
}  // namespace DataCenter
