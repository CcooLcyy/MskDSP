#include "ModbusRTUPointTable.h"

#include <algorithm>

namespace ModbusRTU {

grpc::Status PointTable::Upsert(const google::protobuf::RepeatedPtrField<ModbusRTUProto::Point>& points, bool replace) {
  if (replace) {
    byTag_.clear();
    tagByKey_.clear();
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

grpc::Status PointTable::validatePoint(const ModbusRTUProto::Point& point) const {
  if (point.tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag is required");
  }
  if (point.function() == ModbusRTUProto::FUNCTION_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "function is required");
  }
  if (point.type() == ModbusRTUProto::DATA_TYPE_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "data type is required");
  }
  if (point.address() > 65535) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "address must be <= 65535");
  }
  if (point.function() == ModbusRTUProto::FUNCTION_READ_COILS && point.type() != ModbusRTUProto::DATA_TYPE_BOOL) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "coil point requires BOOL type");
  }
  if (point.function() == ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS && point.type() != ModbusRTUProto::DATA_TYPE_UINT16) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "holding register point requires UINT16 type");
  }
  return grpc::Status::OK;
}

grpc::Status PointTable::insertOrUpdatePoint(const ModbusRTUProto::Point& point) {
  auto existingTag = byTag_.find(point.tag());
  PointKey key{point.function(), point.address()};
  auto existingKey = tagByKey_.find(key);

  if (existingTag == byTag_.end() && existingKey == tagByKey_.end()) {
    Point p;
    p.tag = point.tag();
    p.function = point.function();
    p.address = point.address();
    p.type = point.type();
    p.scale = point.scale();
    p.offset = point.offset();
    byTag_.emplace(p.tag, p);
    tagByKey_.emplace(key, p.tag);
    return grpc::Status::OK;
  }

  if (existingTag != byTag_.end()) {
    if (existingTag->second.function != point.function() || existingTag->second.address != point.address()) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "tag is already mapped to a different address");
    }
  }
  if (existingKey != tagByKey_.end() && existingKey->second != point.tag()) {
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "address is already mapped to a different tag");
  }

  Point p;
  p.tag = point.tag();
  p.function = point.function();
  p.address = point.address();
  p.type = point.type();
  p.scale = point.scale();
  p.offset = point.offset();
  byTag_[p.tag] = p;
  tagByKey_[key] = p.tag;
  return grpc::Status::OK;
}

std::optional<PointTable::Point> PointTable::FindByTag(const std::string& tag) const {
  auto it = byTag_.find(tag);
  if (it == byTag_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<PointTable::Point> PointTable::Points() const {
  std::vector<Point> points;
  points.reserve(byTag_.size());
  for (const auto& [_, point] : byTag_) {
    points.emplace_back(point);
  }
  std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
    return a.tag < b.tag;
  });
  return points;
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

void PointTable::ToProto(const std::string& connName, ModbusRTUProto::PointTable* out) const {
  if (out == nullptr) {
    return;
  }
  out->Clear();
  out->set_conn_name(connName);
  auto points = Points();
  for (const auto& point : points) {
    auto* dst = out->add_points();
    dst->set_tag(point.tag);
    dst->set_function(point.function);
    dst->set_address(point.address);
    dst->set_type(point.type);
    dst->set_scale(point.scale);
    dst->set_offset(point.offset);
  }
}

}  // namespace ModbusRTU
