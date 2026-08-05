#include "IEC61850MmsBer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <string_view>

namespace IEC61850 {
namespace {

grpc::Status BerError(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::DATA_LOSS,
                      std::format("IEC61850 MMS BER报文无效: {}", reason));
}

bool IsNegative(std::span<const std::uint8_t> value) noexcept {
  return !value.empty() && (value.front() & 0x80) != 0;
}

}  // namespace

grpc::Status ReadBerTlv(std::span<const std::uint8_t> input,
                        std::size_t* offset, BerTlvView* tlv) {
  if (offset == nullptr || tlv == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS BER读取参数为空");
  }
  *tlv = {};
  if (*offset >= input.size()) {
    return BerError("TLV标签缺失");
  }
  const auto begin = *offset;
  const auto identifierOctet = input[(*offset)++];
  std::uint32_t tagNumber = identifierOctet & 0x1f;
  if (tagNumber == 0x1f) {
    tagNumber = 0;
    bool terminated = false;
    for (std::size_t count = 0; count < 5; ++count) {
      if (*offset >= input.size()) {
        return BerError("高标签缺少标签号字节");
      }
      const auto byte = input[(*offset)++];
      if (count == 0 && byte == 0x80) {
        return BerError("高标签包含非规范前导字节");
      }
      if (tagNumber > (0x1fffffffU >> 7)) {
        return BerError("高标签号溢出");
      }
      tagNumber = (tagNumber << 7) | (byte & 0x7f);
      if ((byte & 0x80) == 0) {
        terminated = true;
        break;
      }
    }
    if (!terminated || tagNumber < 31) {
      return BerError("高标签号编码无效");
    }
  }
  if (*offset >= input.size()) {
    return BerError("TLV长度缺失");
  }
  const auto firstLength = input[(*offset)++];
  std::size_t length = firstLength;
  if ((firstLength & 0x80) != 0) {
    const auto byteCount = static_cast<std::size_t>(firstLength & 0x7f);
    if (byteCount == 0) {
      return BerError("不允许使用无限长度编码");
    }
    if (byteCount > sizeof(std::size_t) ||
        byteCount > input.size() - *offset) {
      return BerError("BER长度字段超出报文边界");
    }
    if (input[*offset] == 0) {
      return BerError("BER长度字段不是规范编码");
    }
    length = 0;
    for (std::size_t index = 0; index < byteCount; ++index) {
      if (length > (std::numeric_limits<std::size_t>::max() >> 8)) {
        return BerError("BER长度计算溢出");
      }
      length = (length << 8) | input[(*offset)++];
    }
    if (length < 0x80) {
      return BerError("BER短长度使用了非规范长格式");
    }
  }
  if (length > input.size() - *offset) {
    return BerError("TLV值超出报文边界");
  }
  tlv->tag = identifierOctet;
  tlv->tagNumber = tagNumber;
  tlv->identifierOctet = identifierOctet;
  tlv->value = input.subspan(*offset, length);
  *offset += length;
  tlv->encodedSize = *offset - begin;
  return grpc::Status::OK;
}

grpc::Status ReadBerBoolean(std::span<const std::uint8_t> value,
                            bool* result) {
  if (result == nullptr || value.size() != 1) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS BER布尔值参数无效");
  }
  *result = value.front() != 0;
  return grpc::Status::OK;
}

grpc::Status ReadBerUnsigned(std::span<const std::uint8_t> value,
                             std::uint64_t* result) {
  if (result == nullptr || value.empty() ||
      value.size() > sizeof(*result) + 1) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS BER无符号整数参数无效");
  }
  bool hasPositiveSignOctet = false;
  if (value.size() > 1 && value.front() == 0) {
    if ((value[1] & 0x80) == 0) {
      return BerError("无符号整数包含非规范前导零");
    }
    hasPositiveSignOctet = true;
    value = value.subspan(1);
  }
  if (!hasPositiveSignOctet && (value.front() & 0x80) != 0) {
    return BerError("无符号整数符号位无效");
  }
  std::uint64_t converted = 0;
  for (const auto byte : value) {
    if (converted > (std::numeric_limits<std::uint64_t>::max() >> 8)) {
      return BerError("无符号整数溢出");
    }
    converted = (converted << 8) | byte;
  }
  *result = converted;
  return grpc::Status::OK;
}

grpc::Status ReadBerSigned(std::span<const std::uint8_t> value,
                           std::int64_t* result) {
  if (result == nullptr || value.empty() || value.size() > sizeof(*result)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS BER有符号整数参数无效");
  }
  if (value.size() > 1) {
    const bool redundant =
        (value.front() == 0x00 && !IsNegative(value.subspan(1))) ||
        (value.front() == 0xff && IsNegative(value.subspan(1)));
    if (redundant) {
      return BerError("有符号整数包含非规范符号扩展");
    }
  }
  std::uint64_t bits = 0;
  for (const auto byte : value) {
    bits = (bits << 8) | byte;
  }
  if (IsNegative(value) && value.size() < sizeof(bits)) {
    bits |= ~std::uint64_t{0} << (value.size() * 8);
  }
  *result = static_cast<std::int64_t>(bits);
  return grpc::Status::OK;
}

grpc::Status ReadBerOid(std::span<const std::uint8_t> value,
                        std::span<std::uint32_t> arcs,
                        std::size_t* arcCount) {
  if (arcCount == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS BER OID输出长度参数为空");
  }
  *arcCount = 0;
  if (value.empty()) {
    return BerError("OID值为空");
  }
  std::size_t offset = 0;
  std::array<std::uint32_t, 32> decoded{};
  std::size_t count = 0;
  std::uint64_t component = 0;
  bool continuation = false;
  bool first = true;
  for (;;) {
    if (offset >= value.size()) {
      if (continuation) {
        return BerError("OID最后一项缺少结束字节");
      }
      break;
    }
    const auto byte = value[offset++];
    if (component > (std::numeric_limits<std::uint64_t>::max() >> 7)) {
      return BerError("OID组件溢出");
    }
    component = (component << 7) | (byte & 0x7f);
    if ((byte & 0x80) != 0) {
      continuation = true;
      continue;
    }
    continuation = false;
    if (component > std::numeric_limits<std::uint32_t>::max()) {
      return BerError("OID组件超出下位机范围");
    }
    if (first) {
      const auto firstArc = component < 40 ? 0u : component < 80 ? 1u : 2u;
      const auto secondArc = component - firstArc * 40;
      if (count + 2 > decoded.size()) {
        return BerError("OID组件数量超出上限");
      }
      decoded[count++] = firstArc;
      decoded[count++] = static_cast<std::uint32_t>(secondArc);
      first = false;
    } else {
      if (count >= decoded.size()) {
        return BerError("OID组件数量超出上限");
      }
      decoded[count++] = static_cast<std::uint32_t>(component);
    }
    component = 0;
    if (offset == value.size()) {
      break;
    }
  }
  if (first || count > arcs.size()) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "IEC61850 MMS BER OID输出缓冲不足");
  }
  std::copy_n(decoded.begin(), count, arcs.begin());
  *arcCount = count;
  return grpc::Status::OK;
}

bool BerWriter::Byte(std::uint8_t value) noexcept {
  if (offset_ >= output_.size()) {
    return false;
  }
  output_[offset_++] = value;
  return true;
}

bool BerWriter::Length(std::size_t value) noexcept {
  if (value < 0x80) {
    return Byte(static_cast<std::uint8_t>(value));
  }
  std::array<std::uint8_t, sizeof(value)> encoded{};
  std::size_t count = 0;
  while (value != 0) {
    encoded[encoded.size() - ++count] = static_cast<std::uint8_t>(value);
    value >>= 8;
  }
  if (!Byte(static_cast<std::uint8_t>(0x80 | count))) {
    return false;
  }
  for (std::size_t index = encoded.size() - count; index < encoded.size();
       ++index) {
    if (!Byte(encoded[index])) {
      return false;
    }
  }
  return true;
}

bool BerWriter::Tlv(std::uint32_t tag,
                    std::span<const std::uint8_t> value) noexcept {
  const auto oldOffset = offset_;
  if (!Tag(tag) || !Length(value.size()) ||
      value.size() > output_.size() - offset_) {
    offset_ = oldOffset;
    return false;
  }
  std::copy(value.begin(), value.end(), output_.begin() + offset_);
  offset_ += value.size();
  return true;
}

bool BerWriter::Tag(std::uint32_t tag) noexcept {
  if (tag <= 0xff) {
    return Byte(static_cast<std::uint8_t>(tag));
  }
  const auto identifier = static_cast<std::uint8_t>(tag >> 24);
  const auto tagNumber = tag & 0x00ffffffU;
  if ((identifier & 0x1f) != 0x1f || tagNumber < 31) {
    return false;
  }
  if (!Byte(static_cast<std::uint8_t>(identifier | 0x1f))) {
    return false;
  }
  std::array<std::uint8_t, 5> encoded{};
  std::size_t count = 0;
  std::uint32_t remaining = tagNumber;
  do {
    encoded[encoded.size() - ++count] =
        static_cast<std::uint8_t>(remaining & 0x7f);
    remaining >>= 7;
  } while (remaining != 0 && count < encoded.size());
  if (remaining != 0) {
    return false;
  }
  for (std::size_t index = encoded.size() - count; index < encoded.size();
       ++index) {
    auto value = encoded[index];
    if (index + 1 != encoded.size()) {
      value = static_cast<std::uint8_t>(value | 0x80);
    }
    if (!Byte(value)) {
      return false;
    }
  }
  return true;
}

bool BerWriter::Boolean(std::uint8_t tag, bool value) noexcept {
  const std::array<std::uint8_t, 1> encoded{
      static_cast<std::uint8_t>(value ? 0xff : 0x00)};
  return Tlv(tag, encoded);
}

bool BerWriter::Unsigned(std::uint8_t tag, std::uint64_t value) noexcept {
  std::array<std::uint8_t, sizeof(value) + 1> encoded{};
  std::size_t first = encoded.size() - sizeof(value);
  for (std::size_t index = encoded.size(); index > first; --index) {
    encoded[index - 1] = static_cast<std::uint8_t>(value);
    value >>= 8;
  }
  while (first + 1 < encoded.size() && encoded[first] == 0) {
    ++first;
  }
  if ((encoded[first] & 0x80) != 0) {
    if (first == 0) {
      return false;
    }
    encoded[--first] = 0;
  }
  return Tlv(tag, std::span<const std::uint8_t>(encoded.data() + first,
                                                 encoded.size() - first));
}

bool BerWriter::Signed(std::uint8_t tag, std::int64_t value) noexcept {
  std::array<std::uint8_t, sizeof(value)> encoded{};
  std::uint64_t bits = static_cast<std::uint64_t>(value);
  for (std::size_t index = encoded.size(); index > 0; --index) {
    encoded[index - 1] = static_cast<std::uint8_t>(bits);
    bits >>= 8;
  }
  std::size_t first = 0;
  while (first + 1 < encoded.size()) {
    const bool negative = (encoded[first] & 0x80) != 0;
    const bool redundant =
        (encoded[first] == 0x00 && (encoded[first + 1] & 0x80) == 0) ||
        (encoded[first] == 0xff && (encoded[first + 1] & 0x80) != 0);
    if (!negative || !redundant) {
      if (value >= 0 && encoded[first] == 0x00 &&
          (encoded[first + 1] & 0x80) == 0) {
        ++first;
        continue;
      }
      if (value < 0 && encoded[first] == 0xff &&
          (encoded[first + 1] & 0x80) != 0) {
        ++first;
        continue;
      }
      break;
    }
    ++first;
  }
  return Tlv(tag, std::span<const std::uint8_t>(encoded.data() + first,
                                                 encoded.size() - first));
}

bool BerWriter::Oid(std::uint8_t tag,
                    std::span<const std::uint32_t> arcs) noexcept {
  if (arcs.size() < 2 || arcs[0] > 2 ||
      (arcs[0] < 2 && arcs[1] > 39)) {
    return false;
  }
  std::array<std::uint8_t, 256> encoded{};
  std::size_t size = 0;
  auto appendComponent = [&encoded, &size](std::uint64_t component) noexcept {
    std::array<std::uint8_t, 10> bytes{};
    std::size_t count = 0;
    do {
      bytes[bytes.size() - ++count] =
          static_cast<std::uint8_t>(component & 0x7f);
      component >>= 7;
    } while (component != 0 && count < bytes.size());
    if (component != 0 || size > encoded.size() - count) {
      return false;
    }
    for (std::size_t index = bytes.size() - count; index < bytes.size();
         ++index) {
      auto value = bytes[index];
      if (index + 1 != bytes.size()) {
        value = static_cast<std::uint8_t>(value | 0x80);
      }
      encoded[size++] = value;
    }
    return true;
  };
  if (!appendComponent(static_cast<std::uint64_t>(arcs[0]) * 40 + arcs[1])) {
    return false;
  }
  for (std::size_t index = 2; index < arcs.size(); ++index) {
    if (!appendComponent(arcs[index])) {
      return false;
    }
  }
  return Tlv(tag, std::span<const std::uint8_t>(encoded.data(), size));
}

}  // namespace IEC61850
