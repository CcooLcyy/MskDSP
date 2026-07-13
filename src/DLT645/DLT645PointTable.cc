#include "DLT645PointTable.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_set>

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

bool requireDataLen(uint32_t dataLen, uint32_t expected) {
  return dataLen == expected;
}

bool isBitBool(const DLT645Proto::Point& point) {
  return point.type() == DLT645Proto::DATA_TYPE_BOOL && point.has_bit_index();
}

bool isBitBool(const DLT645Proto::BlockItem& item) {
  return item.type() == DLT645Proto::DATA_TYPE_BOOL && item.has_bit_index();
}

bool validateBitPosition(uint32_t dataLen, uint32_t byteIndex, uint32_t bitIndex) {
  return byteIndex < dataLen && bitIndex < 8;
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

grpc::Status PointTable::Upsert(const google::protobuf::RepeatedPtrField<DLT645Proto::Point>& points,
                                const google::protobuf::RepeatedPtrField<DLT645Proto::Block>& blocks,
                                bool replace) {
  if (replace) {
    byTag_.clear();
    tagsByDi_.clear();
    blocks_.clear();
    blockDiSet_.clear();
    blockItemByTag_.clear();
    blockTags_.clear();
  }

  for (const auto& point : points) {
    auto status = validatePoint(point);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& block : blocks) {
    auto status = validateBlock(block);
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
  for (const auto& block : blocks) {
    auto status = insertOrUpdateBlock(block);
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

const std::vector<PointTable::Block>& PointTable::Blocks() const {
  return blocks_;
}

std::vector<std::string> PointTable::Tags() const {
  std::unordered_set<std::string> unique;
  unique.reserve(byTag_.size() + blockTags_.size());
  for (const auto& item : byTag_) {
    unique.insert(item.first);
  }
  for (const auto& tag : blockTags_) {
    unique.insert(tag);
  }
  std::vector<std::string> tags;
  tags.reserve(unique.size());
  for (const auto& tag : unique) {
    tags.push_back(tag);
  }
  std::sort(tags.begin(), tags.end());
  return tags;
}

const std::unordered_set<std::string>& PointTable::BlockTags() const {
  return blockTags_;
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
    if (point.byteIndex.has_value()) {
      outPoint->set_byte_index(point.byteIndex.value());
    }
    if (point.bitIndex.has_value()) {
      outPoint->set_bit_index(point.bitIndex.value());
    }
  }
  for (const auto& block : blocks_) {
    auto* outBlock = out->add_blocks();
    outBlock->set_block_di(block.diText);
    outBlock->set_block_data_len(block.dataLen);
    for (const auto& item : block.items) {
      const auto& point = item.point;
      auto* outItem = outBlock->add_items();
      outItem->set_tag(point.tag);
      outItem->set_data_len(point.dataLen);
      outItem->set_type(point.type);
      outItem->set_access(point.access);
      outItem->set_scale(point.scale);
      outItem->set_offset(point.offset);
      outItem->set_deadband(point.deadband);
      outItem->set_trim_right_space(item.trimRightSpace);
      if (point.byteIndex.has_value()) {
        outItem->set_byte_index(point.byteIndex.value());
      }
      if (point.bitIndex.has_value()) {
        outItem->set_bit_index(point.bitIndex.value());
      }
    }
  }
}

bool PointTable::isSameDefinition(const Point& lhs, const Point& rhs) {
  return lhs.dataLen == rhs.dataLen &&
      lhs.type == rhs.type &&
      lhs.scale == rhs.scale &&
      lhs.offset == rhs.offset &&
      lhs.deadband == rhs.deadband &&
      lhs.byteIndex == rhs.byteIndex &&
      lhs.bitIndex == rhs.bitIndex;
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
  if (isBitBool(point)) {
    const uint32_t byteIndex = point.has_byte_index() ? point.byte_index() : 0;
    if (!validateBitPosition(point.data_len(), byteIndex, point.bit_index())) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "BOOL 点位 bit 位置超出 data_len 范围");
    }
    if (point.access() != DLT645Proto::ACCESS_READ_ONLY) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "BOOL bit 点位仅支持只读遥信");
    }
  } else if (point.type() == DLT645Proto::DATA_TYPE_BOOL && point.has_byte_index()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "byte_index 需要和 bit_index 同时配置");
  } else if (point.type() == DLT645Proto::DATA_TYPE_BOOL && !requireDataLen(point.data_len(), 1)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "BOOL 点位 data_len 必须为 1");
  }
  if (point.type() == DLT645Proto::DATA_TYPE_UINT16 && !requireDataLen(point.data_len(), 2)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "UINT16 点位 data_len 必须为 2");
  }
  if (point.type() == DLT645Proto::DATA_TYPE_UINT32 && !requireDataLen(point.data_len(), 4)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "UINT32 点位 data_len 必须为 4");
  }
  if (point.type() == DLT645Proto::DATA_TYPE_FLOAT && !requireDataLen(point.data_len(), 4)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "FLOAT 点位 data_len 必须为 4");
  }
  return grpc::Status::OK;
}

grpc::Status PointTable::validateBlockItem(const DLT645Proto::BlockItem& item) {
  if (item.tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块子项 tag 不能为空");
  }
  if (item.data_len() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块子项 data_len 不能为空");
  }
  if (item.type() == DLT645Proto::DATA_TYPE_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块子项 type 不能为空");
  }
  if (item.access() == DLT645Proto::ACCESS_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块子项 access 不能为空");
  }
  if (item.deadband() < 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块子项 deadband 不能为负数");
  }
  if (isBitBool(item)) {
    const uint32_t byteIndex = item.has_byte_index() ? item.byte_index() : 0;
    if (!validateBitPosition(item.data_len(), byteIndex, item.bit_index())) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块 BOOL 子项 bit 位置超出 data_len 范围");
    }
    if (item.access() != DLT645Proto::ACCESS_READ_ONLY) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块 BOOL bit 子项仅支持只读遥信");
    }
  } else if (item.type() == DLT645Proto::DATA_TYPE_BOOL && item.has_byte_index()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块 BOOL 子项 byte_index 需要和 bit_index 同时配置");
  } else if (item.type() == DLT645Proto::DATA_TYPE_BOOL && !requireDataLen(item.data_len(), 1)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块 BOOL 子项 data_len 必须为 1");
  }
  if (item.type() == DLT645Proto::DATA_TYPE_UINT16 && !requireDataLen(item.data_len(), 2)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块 UINT16 子项 data_len 必须为 2");
  }
  if (item.type() == DLT645Proto::DATA_TYPE_UINT32 && !requireDataLen(item.data_len(), 4)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块 UINT32 子项 data_len 必须为 4");
  }
  if (item.type() == DLT645Proto::DATA_TYPE_FLOAT && !requireDataLen(item.data_len(), 4)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块 FLOAT 子项 data_len 必须为 4");
  }
  return grpc::Status::OK;
}

grpc::Status PointTable::validateBlock(const DLT645Proto::Block& block) {
  if (block.block_di().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "block_di 不能为空");
  }
  if (block.block_di().size() != 8) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "block_di 必须为 8 位十六进制字符串");
  }
  for (char ch : block.block_di()) {
    if (!isHexChar(ch)) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "block_di 必须为十六进制字符串");
    }
  }
  if (block.block_data_len() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "block_data_len 不能为空");
  }
  if (block.items_size() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块 items 不能为空");
  }
  uint32_t totalLen = 0;
  for (const auto& item : block.items()) {
    auto status = validateBlockItem(item);
    if (!status.ok()) {
      return status;
    }
    totalLen += item.data_len();
  }
  if (totalLen != block.block_data_len()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块长度与子项长度之和不一致");
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
  auto existingDi = tagsByDi_.find(point.di());
  if (existingDi != tagsByDi_.end()) {
    const bool nextBitPoint = point.has_bit_index();
    for (const auto& tag : existingDi->second) {
      if (tag == point.tag()) {
        continue;
      }
      auto tagIt = byTag_.find(tag);
      if (tagIt == byTag_.end()) {
        continue;
      }
      const auto& existingPoint = tagIt->second;
      if (!nextBitPoint || !existingPoint.bitIndex.has_value()) {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "di 已映射到非 bit 点位");
      }
      const uint32_t nextByteIndex = point.has_byte_index() ? point.byte_index() : 0;
      if (existingPoint.byteIndex.value_or(0) == nextByteIndex &&
          existingPoint.bitIndex.value() == point.bit_index()) {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "di 的同一 bit 已映射到其他 tag");
      }
      if (existingPoint.dataLen != point.data_len()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "同 DI bit 点位 data_len 必须一致");
      }
    }
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
  if (point.has_bit_index()) {
    p.byteIndex = point.has_byte_index() ? point.byte_index() : 0;
    p.bitIndex = point.bit_index();
  }

  auto blockIt = blockItemByTag_.find(p.tag);
  if (blockIt != blockItemByTag_.end()) {
    if (!isSameDefinition(blockIt->second.point, p)) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块与单点定义不一致");
    }
    LOG_WARNING("DLT645 点表存在读块写点冲突: tag={}, 读使用数据块, 写使用单点", p.tag);
  }

  byTag_[p.tag] = p;
  auto& tags = tagsByDi_[p.diText];
  if (std::find(tags.begin(), tags.end(), p.tag) == tags.end()) {
    tags.push_back(p.tag);
  }
  LOG_DEBUG("DLT645 点表写入点位: tag={}, 配置DI={}, 发送DI={}, data_len={}, byte_index={}, bit_index={}",
            p.tag,
            p.diText,
            formatHex(p.diBytes),
            p.dataLen,
            p.byteIndex.has_value() ? std::to_string(p.byteIndex.value()) : "-",
            p.bitIndex.has_value() ? std::to_string(p.bitIndex.value()) : "-");
  return grpc::Status::OK;
}

grpc::Status PointTable::insertOrUpdateBlock(const DLT645Proto::Block& block) {
  if (blockDiSet_.find(block.block_di()) != blockDiSet_.end()) {
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "block_di 已存在");
  }

  std::array<uint8_t, 4> di{};
  auto status = parseDi(block.block_di(), &di);
  if (!status.ok()) {
    return status;
  }

  Block blockDef;
  blockDef.diText = block.block_di();
  blockDef.diBytes = di;
  blockDef.dataLen = block.block_data_len();
  blockDef.items.reserve(block.items_size());

  std::unordered_set<std::string> localTags;
  localTags.reserve(static_cast<size_t>(block.items_size()));

  uint32_t offset = 0;
  for (const auto& item : block.items()) {
    if (localTags.find(item.tag()) != localTags.end()) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "数据块内 tag 重复");
    }
    if (blockItemByTag_.find(item.tag()) != blockItemByTag_.end()) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "tag 已在其他数据块中定义");
    }
    localTags.insert(item.tag());

    Point p;
    p.tag = item.tag();
    p.dataLen = item.data_len();
    p.type = item.type();
    p.access = item.access();
    p.scale = item.scale();
    if (p.scale == 0.0) {
      p.scale = 1.0;
    }
    p.offset = item.offset();
    p.deadband = item.deadband();
    if (item.has_bit_index()) {
      p.byteIndex = item.has_byte_index() ? item.byte_index() : 0;
      p.bitIndex = item.bit_index();
    }

    auto pointIt = byTag_.find(p.tag);
    if (pointIt != byTag_.end()) {
      if (!isSameDefinition(pointIt->second, p)) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块与单点定义不一致");
      }
    }

    BlockItem blockItem;
    blockItem.point = p;
    blockItem.offset = offset;
    blockItem.trimRightSpace = item.has_trim_right_space() ? item.trim_right_space() : true;

    blockDef.items.push_back(blockItem);
    offset += p.dataLen;
  }

  if (offset != blockDef.dataLen) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "数据块长度与子项长度之和不一致");
  }

  blocks_.push_back(blockDef);
  blockDiSet_.insert(blockDef.diText);
  for (const auto& item : blockDef.items) {
    blockItemByTag_[item.point.tag] = item;
    blockTags_.insert(item.point.tag);
    auto pointIt = byTag_.find(item.point.tag);
    if (pointIt != byTag_.end()) {
      LOG_WARNING("DLT645 点表存在读块写点冲突: tag={}, 读使用数据块, 写使用单点", item.point.tag);
    }
    LOG_DEBUG("DLT645 数据块写入子项: block_di={}, tag={}, offset={}, data_len={}", blockDef.diText, item.point.tag,
              item.offset, item.point.dataLen);
  }
  LOG_INFO("DLT645 点表写入数据块: block_di={}, 发送DI={}, data_len={}, item_count={}",
           blockDef.diText, formatHex(blockDef.diBytes), blockDef.dataLen, blockDef.items.size());
  return grpc::Status::OK;
}

}  // namespace DLT645
