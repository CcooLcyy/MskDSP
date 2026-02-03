#include "DLT645PointTable.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "Logger.h"

namespace {
bool isHexChar(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

uint8_t hexValue(char ch) {
  if (ch >= '0' && ch <= '9') {
    return static_cast<uint8_t>(ch - '0');
  }
  if (ch >= 'a' && ch <= 'f') {
    return static_cast<uint8_t>(ch - 'a' + 10);
  }
  return static_cast<uint8_t>(ch - 'A' + 10);
}

bool requireDataLen(const DLT645Proto::Point& point, uint32_t expected) {
  return point.data_len() == expected;
}

std::string formatHex(const std::array<uint8_t, 4>& data) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (uint8_t b : data) {
    oss << std::setw(2) << static_cast<int>(b);
  }
  return oss.str();
}
}  // namespace

namespace DLT645 {

grpc::Status PointTable::Upsert(const google::protobuf::RepeatedPtrField<DLT645Proto::Point>& points, bool replace) {
  if (replace) {
    byTag_.clear();
    tagByDi_.clear();
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
  for (const auto& item : byTag_) {
    points.push_back(item.second);
  }
  std::sort(points.begin(), points.end(),
            [](const Point& lhs, const Point& rhs) { return lhs.tag < rhs.tag; });
  return points;
}

std::vector<std::string> PointTable::Tags() const {
  std::vector<std::string> tags;
  tags.reserve(byTag_.size());
  for (const auto& item : byTag_) {
    tags.push_back(item.first);
  }
  std::sort(tags.begin(), tags.end());
  return tags;
}

void PointTable::ToProto(const std::string& connName, DLT645Proto::PointTable* out) const {
  if (out == nullptr) {
    return;
  }
  out->Clear();
  out->set_conn_name(connName);
  for (const auto& item : byTag_) {
    const auto& point = item.second;
    auto* outPoint = out->add_points();
    outPoint->set_tag(point.tag);
    outPoint->set_di(point.diText);
    outPoint->set_data_len(point.dataLen);
    outPoint->set_type(point.type);
    outPoint->set_access(point.access);
    outPoint->set_scale(point.scale);
    outPoint->set_offset(point.offset);
    outPoint->set_deadband(point.deadband);
  }
}

grpc::Status PointTable::validatePoint(const DLT645Proto::Point& point) {
  if (point.tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tag 不能为空");
  }
  if (point.di().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "di 不能为空");
  }
  if (point.di().size() != 8) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "di 必须为 8 位十六进制字符串");
  }
  for (char ch : point.di()) {
    if (!isHexChar(ch)) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "di 必须为十六进制字符串");
    }
  }
  if (point.data_len() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "data_len 不能为空");
  }
  if (point.type() == DLT645Proto::DATA_TYPE_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "type 不能为空");
  }
  if (point.access() == DLT645Proto::ACCESS_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "access 不能为空");
  }
  if (point.deadband() < 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "deadband 不能为负数");
  }
  if (point.type() == DLT645Proto::DATA_TYPE_BOOL && !requireDataLen(point, 1)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "BOOL 点位 data_len 必须为 1");
  }
  if (point.type() == DLT645Proto::DATA_TYPE_UINT16 && !requireDataLen(point, 2)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "UINT16 点位 data_len 必须为 2");
  }
  if (point.type() == DLT645Proto::DATA_TYPE_UINT32 && !requireDataLen(point, 4)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "UINT32 点位 data_len 必须为 4");
  }
  if (point.type() == DLT645Proto::DATA_TYPE_FLOAT && !requireDataLen(point, 4)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "FLOAT 点位 data_len 必须为 4");
  }
  return grpc::Status::OK;
}

bool PointTable::parseHexByte(std::string_view text, uint8_t* out) {
  if (out == nullptr || text.size() != 2) {
    return false;
  }
  if (!isHexChar(text[0]) || !isHexChar(text[1])) {
    return false;
  }
  *out = static_cast<uint8_t>((hexValue(text[0]) << 4) | hexValue(text[1]));
  return true;
}

grpc::Status PointTable::parseDi(const std::string& di, std::array<uint8_t, 4>* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "di 解析失败");
  }
  if (di.size() != 8) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "di 必须为 8 位十六进制字符串");
  }
  for (size_t i = 0; i < 4; ++i) {
    uint8_t value = 0;
    if (!parseHexByte(std::string_view(di).substr(i * 2, 2), &value)) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "di 包含非法十六进制字符");
    }
    (*out)[i] = value;
  }
  std::reverse(out->begin(), out->end());
  return grpc::Status::OK;
}

grpc::Status PointTable::insertOrUpdatePoint(const DLT645Proto::Point& point) {
  std::array<uint8_t, 4> di{};
  auto status = parseDi(point.di(), &di);
  if (!status.ok()) {
    return status;
  }

  auto existingTag = byTag_.find(point.tag());
  if (existingTag != byTag_.end()) {
    if (existingTag->second.diText != point.di()) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "tag 已映射到其他 di");
    }
  }
  auto existingDi = tagByDi_.find(point.di());
  if (existingDi != tagByDi_.end() && existingDi->second != point.tag()) {
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "di 已映射到其他 tag");
  }

  Point p;
  p.tag = point.tag();
  p.diText = point.di();
  p.diBytes = di;
  p.dataLen = point.data_len();
  p.type = point.type();
  p.access = point.access();
  p.scale = point.scale();
  if (p.scale == 0.0) {
    p.scale = 1.0;
  }
  p.offset = point.offset();
  p.deadband = point.deadband();

  byTag_[p.tag] = p;
  tagByDi_[p.diText] = p.tag;
  LOG_DEBUG("DLT645 点表写入点位: tag={}, 配置DI={}, 发送DI={}, data_len={}", p.tag, p.diText, formatHex(p.diBytes),
            p.dataLen);
  return grpc::Status::OK;
}

}  // namespace DLT645
