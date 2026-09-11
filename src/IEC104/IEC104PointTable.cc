#include "IEC104PointTable.h"

#include <algorithm>
#include <format>

#include "Logger.h"

namespace IEC104 {
namespace {

std::expected<mskdsp::numeric::Decimal20, mskdsp::numeric::DecimalError>
pointDecimal(std::string_view decimalText, double legacyValue) {
  return mskdsp::numeric::ParseConfiguredDecimal(decimalText, legacyValue);
}

grpc::Status validateDecimalField(std::string_view fieldName,
                                  std::string_view decimalText,
                                  double legacyValue) {
  auto value = pointDecimal(decimalText, legacyValue);
  if (value) {
    return grpc::Status::OK;
  }
  return grpc::Status(
      grpc::StatusCode::INVALID_ARGUMENT,
      std::format("{} 的十进制配置非法: {}", fieldName,
                  mskdsp::numeric::DecimalErrorMessage(value.error())));
}

void applyEngineeringConfig(const IEC104Proto::Point& source,
                            PointTable::Point* target) {
  if (target == nullptr) {
    return;
  }
  if (source.type() != IEC104Proto::POINT_TYPE_FLOAT) {
    target->scale = mskdsp::numeric::Decimal20::FromInt64(1).value();
    target->offset = mskdsp::numeric::Decimal20{};
    target->deadband = mskdsp::numeric::Decimal20{};
    return;
  }
  target->scale = pointDecimal(source.scale_decimal(), source.scale()).value();
  if (target->scale.IsZero()) {
    target->scale = mskdsp::numeric::Decimal20::FromInt64(1).value();
  }
  target->offset = pointDecimal(source.offset_decimal(), source.offset()).value();
  target->deadband = pointDecimal(source.deadband_decimal(), source.deadband()).value();
  if (!source.scale_decimal().empty() || !source.offset_decimal().empty() ||
      !source.deadband_decimal().empty()) {
    LOG_INFO("IEC104 点表优先使用精确十进制工程量配置: tag={}, ioa={}",
             source.tag(), source.ioa());
  }
}

}  // namespace

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
  auto status = validateDecimalField("scale_decimal", point.scale_decimal(), point.scale());
  if (!status.ok()) {
    return status;
  }
  status = validateDecimalField("offset_decimal", point.offset_decimal(), point.offset());
  if (!status.ok()) {
    return status;
  }
  status = validateDecimalField("deadband_decimal", point.deadband_decimal(), point.deadband());
  if (!status.ok()) {
    return status;
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
  if (point.remote_control_type() != IEC104Proto::REMOTE_CONTROL_TYPE_UNSPECIFIED
      && point.remote_control_type() != IEC104Proto::REMOTE_CONTROL_TYPE_SINGLE
      && point.remote_control_type() != IEC104Proto::REMOTE_CONTROL_TYPE_DOUBLE) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "remote_control_type 不支持");
  }
  if (point.command_execution_mode() != IEC104Proto::COMMAND_EXECUTION_MODE_UNSPECIFIED
      && point.command_execution_mode() != IEC104Proto::COMMAND_EXECUTION_MODE_DIRECT
      && point.command_execution_mode() != IEC104Proto::COMMAND_EXECUTION_MODE_SELECT_EXECUTE) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "command_execution_mode 不支持");
  }
  const auto effectiveBusinessType = point.business_type() == IEC104Proto::POINT_BUSINESS_TYPE_UNSPECIFIED
      ? InferBusinessType(point.ioa(), point.type())
      : point.business_type();
  if (effectiveBusinessType != IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_CONTROL
      && point.remote_control_type() == IEC104Proto::REMOTE_CONTROL_TYPE_DOUBLE) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "非遥控点不能配置双点遥控");
  }
  return grpc::Status::OK;
}

namespace {
void applyRemoteControlOptions(const IEC104Proto::Point& source, PointTable::Point* target) {
  if (target == nullptr) {
    return;
  }
  target->remoteControlType = source.remote_control_type() == IEC104Proto::REMOTE_CONTROL_TYPE_UNSPECIFIED
      ? IEC104Proto::REMOTE_CONTROL_TYPE_SINGLE
      : source.remote_control_type();
  target->commandExecutionMode = source.command_execution_mode() == IEC104Proto::COMMAND_EXECUTION_MODE_UNSPECIFIED
      ? IEC104Proto::COMMAND_EXECUTION_MODE_SELECT_EXECUTE
      : source.command_execution_mode();
  if (target->businessType != IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_CONTROL) {
    target->remoteControlType = IEC104Proto::REMOTE_CONTROL_TYPE_SINGLE;
    target->commandExecutionMode = IEC104Proto::COMMAND_EXECUTION_MODE_SELECT_EXECUTE;
  }
}
}  // namespace

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
    applyEngineeringConfig(point, &p);
    applyRemoteControlOptions(point, &p);
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
  applyEngineeringConfig(point, &p);
  applyRemoteControlOptions(point, &p);
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
    dst->set_scale(*p.scale.ToDouble());
    dst->set_offset(*p.offset.ToDouble());
    dst->set_deadband(*p.deadband.ToDouble());
    dst->set_scale_decimal(p.scale.ToFixedString());
    dst->set_offset_decimal(p.offset.ToFixedString());
    dst->set_deadband_decimal(p.deadband.ToFixedString());
    dst->set_remote_control_type(p.remoteControlType);
    dst->set_command_execution_mode(p.commandExecutionMode);
  }
}

}  // namespace IEC104
