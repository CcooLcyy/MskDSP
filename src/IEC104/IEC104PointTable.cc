#include "IEC104PointTable.h"

#include <algorithm>

namespace IEC104 {

grpc::Status PointTable::Upsert(const google::protobuf::RepeatedPtrField<IEC104Proto::TelemetryPoint>& points, bool replace) {
  if (replace) {
    byTag_.clear();
    tagByIoa_.clear();
  }

  for (const auto& point : points) {
    auto status = validatePoint(point);
    if (!status.ok()) {
      return status;
    }
  }

  for (const auto& point : points) {
    auto status = insertOrUpdatePoint(point);
    if (!status.ok()) {
      return status;
    }
  }
  return grpc::Status::OK;
}

grpc::Status PointTable::validatePoint(const IEC104Proto::TelemetryPoint& point) const {
  if (point.tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag 不能为空");
  }
  if (point.ioa() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "ioa 不能为空");
  }
  if (point.type() == IEC104Proto::TELEMETRY_TYPE_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "telemetry_type 不能为空");
  }
  return grpc::Status::OK;
}

grpc::Status PointTable::insertOrUpdatePoint(const IEC104Proto::TelemetryPoint& point) {
  auto existingTag = byTag_.find(point.tag());
  auto existingIoa = tagByIoa_.find(point.ioa());

  if (existingTag == byTag_.end() && existingIoa == tagByIoa_.end()) {
    Point p;
    p.tag = point.tag();
    p.ioa = point.ioa();
    p.type = point.type();
    p.scale = point.scale();
    p.offset = point.offset();
    byTag_.emplace(p.tag, p);
    tagByIoa_.emplace(p.ioa, p.tag);
    return grpc::Status::OK;
  }

  if (existingTag != byTag_.end() && existingTag->second.ioa != point.ioa()) {
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "tag 已映射到其他 ioa");
  }
  if (existingIoa != tagByIoa_.end() && existingIoa->second != point.tag()) {
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "ioa 已映射到其他 tag");
  }

  Point p;
  p.tag = point.tag();
  p.ioa = point.ioa();
  p.type = point.type();
  p.scale = point.scale();
  p.offset = point.offset();
  byTag_[p.tag] = p;
  tagByIoa_[p.ioa] = p.tag;
  return grpc::Status::OK;
}

std::optional<PointTable::Point> PointTable::FindByTag(const std::string& tag) const {
  auto it = byTag_.find(tag);
  if (it == byTag_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<PointTable::Point> PointTable::FindByIoa(uint32_t ioa) const {
  auto it = tagByIoa_.find(ioa);
  if (it == tagByIoa_.end()) {
    return std::nullopt;
  }
  return FindByTag(it->second);
}

std::vector<std::string> PointTable::Tags() const {
  std::vector<std::string> tags;
  tags.reserve(byTag_.size());
  for (const auto& [tag, _] : byTag_) {
    tags.emplace_back(tag);
  }
  std::sort(tags.begin(), tags.end());
  return tags;
}

void PointTable::ToProto(const std::string& connName, IEC104Proto::PointTable* out) const {
  if (out == nullptr) {
    return;
  }
  out->Clear();
  out->set_conn_name(connName);
  auto tags = Tags();
  for (const auto& tag : tags) {
    const auto& p = byTag_.at(tag);
    auto* dst = out->add_points();
    dst->set_tag(p.tag);
    dst->set_ioa(p.ioa);
    dst->set_type(p.type);
    dst->set_scale(p.scale);
    dst->set_offset(p.offset);
  }
}

}  // namespace IEC104
