#include "ModbusRTUPointTable.h"

#include <algorithm>

#include "Logger.h"

namespace {
uint32_t defaultRegCount(ModbusRTUProto::DataType type) {
  return type == ModbusRTUProto::DATA_TYPE_UINT32 ? 2u : 1u;
}

bool isValidWordOrder(ModbusRTUProto::WordOrder order) {
  return order == ModbusRTUProto::WORD_ORDER_UNSPECIFIED ||
      order == ModbusRTUProto::WORD_ORDER_HL ||
      order == ModbusRTUProto::WORD_ORDER_LH;
}

bool isValidByteOrder(ModbusRTUProto::ByteOrder order) {
  return order == ModbusRTUProto::BYTE_ORDER_UNSPECIFIED ||
      order == ModbusRTUProto::BYTE_ORDER_AB ||
      order == ModbusRTUProto::BYTE_ORDER_BA;
}

ModbusRTUProto::WordOrder normalizeWordOrder(ModbusRTUProto::WordOrder order) {
  return order == ModbusRTUProto::WORD_ORDER_LH ? order : ModbusRTUProto::WORD_ORDER_HL;
}

ModbusRTUProto::ByteOrder normalizeByteOrder(ModbusRTUProto::ByteOrder order) {
  return order == ModbusRTUProto::BYTE_ORDER_BA ? order : ModbusRTUProto::BYTE_ORDER_AB;
}
}  // namespace

namespace ModbusRTU {

grpc::Status PointTable::Upsert(const google::protobuf::RepeatedPtrField<ModbusRTUProto::Point> &points, bool replace) {
  if (replace) {
    byTag_.clear();
    tagByKey_.clear();
  }

  for (const auto &point : points) {
    auto status = validatePoint(point);
    if (!status.ok()) {
      return status;
    }
  }

  for (const auto &point : points) {
    auto status = insertOrUpdatePoint(point);
    if (!status.ok()) {
      return status;
    }
  }
  return grpc::Status::OK;
}

grpc::Status PointTable::validatePoint(const ModbusRTUProto::Point &point) const {
  if (point.tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag 不能为空");
  }
  if (point.function() == ModbusRTUProto::FUNCTION_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "function 不能为空");
  }
  if (point.type() == ModbusRTUProto::DATA_TYPE_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "type 不能为空");
  }
  if (!isValidWordOrder(point.word_order())) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "word_order 非法");
  }
  if (!isValidByteOrder(point.byte_order())) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "byte_order 非法");
  }
  if (point.default_value_case() == ModbusRTUProto::Point::kDefaultBool &&
      point.type() != ModbusRTUProto::DATA_TYPE_BOOL) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "default_bool 需要 BOOL 类型");
  }
  if (point.default_value_case() == ModbusRTUProto::Point::kDefaultUint16 &&
      point.type() != ModbusRTUProto::DATA_TYPE_UINT16) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "default_uint16 需要 UINT16 类型");
  }
  if (point.default_value_case() == ModbusRTUProto::Point::kDefaultUint32 &&
      point.type() != ModbusRTUProto::DATA_TYPE_UINT32) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "default_uint32 需要 UINT32 类型");
  }
  if (point.default_value_case() == ModbusRTUProto::Point::kDefaultUint16 &&
      point.default_uint16() > 0xFFFFu) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "default_uint16 必须 <= 65535");
  }
  uint32_t regCount = point.reg_count();
  if (regCount == 0) {
    regCount = defaultRegCount(point.type());
  }
  if (regCount != 1 && regCount != 2) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "reg_count 仅支持 1 或 2");
  }
  if (point.address() > 65535) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "address 必须 <= 65535");
  }
  if (regCount > 1 && point.address() >= 0xFFFFu) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "address 超出可用范围（reg_count=2）");
  }
  if (point.function() == ModbusRTUProto::FUNCTION_READ_COILS && point.type() != ModbusRTUProto::DATA_TYPE_BOOL) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "线圈点位需要 BOOL 类型");
  }
  if (point.function() == ModbusRTUProto::FUNCTION_READ_COILS && regCount != 1) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "线圈点位 reg_count 只能为 1");
  }
  if (point.function() == ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS &&
      point.type() != ModbusRTUProto::DATA_TYPE_UINT16 &&
      point.type() != ModbusRTUProto::DATA_TYPE_UINT32) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "保持寄存器点位需要 UINT16 或 UINT32 类型");
  }
  if (point.type() == ModbusRTUProto::DATA_TYPE_UINT16 && regCount != 1) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "UINT16 点位 reg_count 只能为 1");
  }
  if (point.type() == ModbusRTUProto::DATA_TYPE_UINT32 && regCount != 2) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "UINT32 点位 reg_count 只能为 2");
  }
  if (point.deadband() < 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "死区不能为负");
  }
  return grpc::Status::OK;
}

grpc::Status PointTable::insertOrUpdatePoint(const ModbusRTUProto::Point &point) {
  Point p;
  p.tag = point.tag();
  p.function = point.function();
  p.address = point.address();
  p.type = point.type();
  p.regCount = point.reg_count() == 0 ? defaultRegCount(point.type()) : point.reg_count();
  p.wordOrder = normalizeWordOrder(point.word_order());
  p.byteOrder = normalizeByteOrder(point.byte_order());
  p.scale = point.scale();
  if (p.scale == 0.0) {
    p.scale = 1.0;
  }
  p.offset = point.offset();
  p.deadband = point.deadband();
  if (point.default_value_case() == ModbusRTUProto::Point::kDefaultBool) {
    p.defaultBool = point.default_bool();
    p.defaultUInt16.reset();
    p.defaultUInt32.reset();
  } else if (point.default_value_case() == ModbusRTUProto::Point::kDefaultUint16) {
    p.defaultUInt16 = static_cast<uint16_t>(point.default_uint16());
    p.defaultBool.reset();
    p.defaultUInt32.reset();
  } else if (point.default_value_case() == ModbusRTUProto::Point::kDefaultUint32) {
    p.defaultUInt32 = point.default_uint32();
    p.defaultBool.reset();
    p.defaultUInt16.reset();
  } else {
    p.defaultBool.reset();
    p.defaultUInt16.reset();
    p.defaultUInt32.reset();
  }

  std::vector<PointKey> keys;
  keys.reserve(p.regCount);
  for (uint32_t i = 0; i < p.regCount; ++i) {
    keys.push_back(PointKey{p.function, p.address + i});
  }

  auto existingTag = byTag_.find(p.tag);
  if (existingTag != byTag_.end()) {
    if (existingTag->second.function != p.function || existingTag->second.address != p.address) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "tag 已映射到其他地址");
    }
  }
  for (const auto &key : keys) {
    auto existingKey = tagByKey_.find(key);
    if (existingKey != tagByKey_.end() && existingKey->second.tag != p.tag) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "地址已映射到其他 tag");
    }
  }

  if (existingTag != byTag_.end()) {
    const auto oldRegCount = existingTag->second.regCount;
    for (uint32_t i = 0; i < oldRegCount; ++i) {
      PointKey oldKey{existingTag->second.function, existingTag->second.address + i};
      auto it = tagByKey_.find(oldKey);
      if (it != tagByKey_.end() && it->second.tag == p.tag) {
        tagByKey_.erase(it);
      }
    }
  }

  byTag_[p.tag] = p;
  for (uint32_t i = 0; i < p.regCount; ++i) {
    tagByKey_[keys[i]] = AddressEntry{p.tag, i};
  }
  LOG_DEBUG("ModbusRTU 点表写入点位: tag={}, address={}, reg_count={}", p.tag, p.address, p.regCount);
  return grpc::Status::OK;
}

std::optional<PointTable::Point> PointTable::FindByTag(const std::string &tag) const {
  auto it = byTag_.find(tag);
  if (it == byTag_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<PointTable::Point> PointTable::FindByAddress(ModbusRTUProto::FunctionCode function, uint32_t address) const {
  auto slot = FindRegisterByAddress(function, address);
  if (!slot.has_value()) {
    return std::nullopt;
  }
  return slot->point;
}

std::optional<PointTable::RegisterLookup> PointTable::FindRegisterByAddress(ModbusRTUProto::FunctionCode function, uint32_t address) const {
  PointKey key{function, address};
  auto it = tagByKey_.find(key);
  if (it == tagByKey_.end()) {
    return std::nullopt;
  }
  auto tagIt = byTag_.find(it->second.tag);
  if (tagIt == byTag_.end()) {
    return std::nullopt;
  }
  RegisterLookup out;
  out.point = tagIt->second;
  out.wordIndex = it->second.wordIndex;
  return out;
}

std::vector<PointTable::Point> PointTable::Points() const {
  std::vector<Point> points;
  points.reserve(byTag_.size());
  for (const auto &[_, point] : byTag_) {
    points.emplace_back(point);
  }
  std::sort(points.begin(), points.end(), [](const Point &a, const Point &b) {
    return a.tag < b.tag;
  });
  return points;
}

std::vector<std::string> PointTable::Tags() const {
  std::vector<std::string> tags;
  tags.reserve(byTag_.size());
  for (const auto &[tag, _] : byTag_) {
    tags.emplace_back(tag);
  }
  std::sort(tags.begin(), tags.end());
  return tags;
}

void PointTable::ToProto(const std::string &connName, ModbusRTUProto::PointTable *out) const {
  if (out == nullptr) {
    return;
  }
  out->Clear();
  out->set_conn_name(connName);
  auto points = Points();
  for (const auto &point : points) {
    auto *dst = out->add_points();
    dst->set_tag(point.tag);
    dst->set_function(point.function);
    dst->set_address(point.address);
    dst->set_type(point.type);
    if (point.type == ModbusRTUProto::DATA_TYPE_UINT16) {
      dst->set_reg_count(point.regCount);
      dst->set_byte_order(point.byteOrder);
    } else if (point.type == ModbusRTUProto::DATA_TYPE_UINT32) {
      dst->set_reg_count(point.regCount);
      dst->set_word_order(point.wordOrder);
      dst->set_byte_order(point.byteOrder);
    }
    dst->set_scale(point.scale);
    dst->set_offset(point.offset);
    dst->set_deadband(point.deadband);
    if (point.defaultBool.has_value()) {
      dst->set_default_bool(point.defaultBool.value());
    } else if (point.defaultUInt16.has_value()) {
      dst->set_default_uint16(point.defaultUInt16.value());
    } else if (point.defaultUInt32.has_value()) {
      dst->set_default_uint32(point.defaultUInt32.value());
    }
  }
}

}  // namespace ModbusRTU
