#include "IEC104PointTable.h"

#include <algorithm>

#include "Logger.h"

namespace IEC104 {

IEC104Proto::PointBusinessType PointTable::InferBusinessType(
    uint32_t ioa, IEC104Proto::PointType type) {
  (void)type;
  if (ioa >= 1 && ioa <= 0x4000) {
    return IEC104Proto::POINT_BUSINESS_TYPE_TELEINDICATION;
  }
  if (ioa >= 0x4001 && ioa <= 0x5000) {
    return IEC104Proto::POINT_BUSINESS_TYPE_TELEMETRY;
  }
  if (ioa >= 0x6001 && ioa <= 0x6100) {
    return IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_CONTROL;
  }
  if (ioa >= 0x6201 && ioa <= 0x6400) {
    return IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_ADJUST;
  }
  if (ioa >= 0xA000 && ioa <= 0xBFFF) {
    return IEC104Proto::POINT_BUSINESS_TYPE_PARAMETER;
  }
  return IEC104Proto::POINT_BUSINESS_TYPE_UNSPECIFIED;
}

bool PointTable::IsSimulationBusinessType(IEC104Proto::PointBusinessType businessType) {
  return businessType == IEC104Proto::POINT_BUSINESS_TYPE_TELEINDICATION
      || businessType == IEC104Proto::POINT_BUSINESS_TYPE_TELEMETRY;
}

grpc::Status PointTable::Upsert(const google::protobuf::RepeatedPtrField<IEC104Proto::Point>& points, bool replace) {
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

grpc::Status PointTable::validatePoint(const IEC104Proto::Point& point) const {
  if (point.tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag 不能为空");
  }
  if (point.ioa() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "ioa 不能为空");
  }
  if (point.type() == IEC104Proto::POINT_TYPE_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_type 不能为空");
  }
  if (point.type() != IEC104Proto::POINT_TYPE_FLOAT && point.type() != IEC104Proto::POINT_TYPE_SINGLE) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_type 不支持");
  }
  if (point.deadband() < 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "死区不能为负");
  }
  switch (point.business_type()) {
  case IEC104Proto::POINT_BUSINESS_TYPE_UNSPECIFIED:
  case IEC104Proto::POINT_BUSINESS_TYPE_TELEINDICATION:
  case IEC104Proto::POINT_BUSINESS_TYPE_TELEMETRY:
  case IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_ADJUST:
  case IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_CONTROL:
  case IEC104Proto::POINT_BUSINESS_TYPE_PARAMETER:
    break;
  default:
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "business_type 不支持");
  }
  return grpc::Status::OK;
}

grpc::Status PointTable::insertOrUpdatePoint(const IEC104Proto::Point& point) {
  auto existingTag = byTag_.find(point.tag());
  auto existingIoa = tagByIoa_.find(point.ioa());

  if (existingTag == byTag_.end() && existingIoa == tagByIoa_.end()) {
    Point p;
    p.tag = point.tag();
    p.ioa = point.ioa();
    p.type = point.type();
    const bool inferredBusinessType =
        point.business_type() == IEC104Proto::POINT_BUSINESS_TYPE_UNSPECIFIED;
    p.businessType = inferredBusinessType ? InferBusinessType(p.ioa, p.type)
                                          : point.business_type();
    if (inferredBusinessType) {
      LOG_DEBUG("IEC104 按 IOA 推导点表业务类型: tag={}, ioa=0x{:06X}, business_type={}",
                p.tag, p.ioa, static_cast<int>(p.businessType));
    }
    if (p.type == IEC104Proto::POINT_TYPE_FLOAT) {
      p.scale = point.scale();
      if (p.scale == 0.0) {
        p.scale = 1.0;
      }
      p.offset = point.offset();
      p.deadband = point.deadband();
    } else {
      p.scale = 1.0;
      p.offset = 0.0;
      p.deadband = 0.0;
    }
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
  const bool inferredBusinessType =
      point.business_type() == IEC104Proto::POINT_BUSINESS_TYPE_UNSPECIFIED;
  p.businessType = inferredBusinessType ? InferBusinessType(p.ioa, p.type)
                                        : point.business_type();
  if (inferredBusinessType) {
    LOG_DEBUG("IEC104 按 IOA 推导点表业务类型: tag={}, ioa=0x{:06X}, business_type={}",
              p.tag, p.ioa, static_cast<int>(p.businessType));
  }
  if (p.type == IEC104Proto::POINT_TYPE_FLOAT) {
    p.scale = point.scale();
    if (p.scale == 0.0) {
      p.scale = 1.0;
    }
    p.offset = point.offset();
    p.deadband = point.deadband();
  } else {
    p.scale = 1.0;
    p.offset = 0.0;
    p.deadband = 0.0;
  }
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
    dst->set_business_type(p.businessType);
    dst->set_scale(p.scale);
    dst->set_offset(p.offset);
    dst->set_deadband(p.deadband);
  }
}

}  // namespace IEC104
