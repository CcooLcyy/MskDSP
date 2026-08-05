#include "IEC61850RawProtocolStack.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IEC61850RawEthernet.h"
#include "IEC61850GoosePublisher.h"
#include "IEC61850MmsWorker.h"
#include "Logger.h"

namespace IEC61850 {
namespace {

constexpr std::uint16_t kGooseEtherType = 0x88b8;
constexpr std::uint16_t kSvEtherType = 0x88ba;
constexpr std::size_t kEthernetFrameCapacity = 2048;

struct BerTlv {
  std::uint8_t tag = 0;
  std::span<const std::uint8_t> value;
};

bool ReadTlv(std::span<const std::uint8_t> input, std::size_t* offset,
             BerTlv* tlv) noexcept {
  if (offset == nullptr || tlv == nullptr || *offset >= input.size()) {
    return false;
  }
  const auto tag = input[(*offset)++];
  if (*offset >= input.size()) {
    return false;
  }
  const auto firstLength = input[(*offset)++];
  std::size_t length = firstLength;
  if ((firstLength & 0x80) != 0) {
    const auto count = static_cast<std::size_t>(firstLength & 0x7f);
    if (count == 0 || count > sizeof(std::size_t) ||
        count > input.size() - *offset || input[*offset] == 0) {
      return false;
    }
    length = 0;
    for (std::size_t index = 0; index < count; ++index) {
      if (length > (std::numeric_limits<std::size_t>::max() >> 8)) {
        return false;
      }
      length = (length << 8) | input[(*offset)++];
    }
    if (length < 0x80) {
      return false;
    }
  }
  if (length > input.size() - *offset) {
    return false;
  }
  tlv->tag = tag;
  tlv->value = input.subspan(*offset, length);
  *offset += length;
  return true;
}

std::optional<std::uint64_t> ReadUnsigned(std::span<const std::uint8_t> value) {
  if (value.empty() || value.size() > sizeof(std::uint64_t) + 1) {
    return std::nullopt;
  }
  bool hasPositiveSignOctet = false;
  if (value.size() > 1 && value.front() == 0) {
    if ((value[1] & 0x80) == 0) {
      return std::nullopt;
    }
    hasPositiveSignOctet = true;
    value = value.subspan(1);
  }
  if (!hasPositiveSignOctet && (value.front() & 0x80) != 0) {
    return std::nullopt;
  }
  std::uint64_t result = 0;
  for (const auto byte : value) {
    if (result > (std::numeric_limits<std::uint64_t>::max() >> 8)) {
      return std::nullopt;
    }
    result = (result << 8) | byte;
  }
  return result;
}

// GOOSE整数域使用BER INTEGER。BER允许正整数保留一个或多个冗余正号
// 八位组，现场设备常见的TTL等字段会以00 64这样的形式编码；这里只
// 在GOOSE头部使用该兼容解码，仍限制总长度和非负范围。
std::optional<std::uint64_t> ReadGooseUnsigned(
    std::span<const std::uint8_t> value) {
  if (value.empty() || value.size() > sizeof(std::uint64_t) + 1) {
    return std::nullopt;
  }
  while (value.size() > 1 && value.front() == 0 &&
         (value[1] & 0x80) == 0) {
    value = value.subspan(1);
  }
  if ((value.front() & 0x80) != 0) {
    return std::nullopt;
  }
  std::uint64_t result = 0;
  for (const auto byte : value) {
    if (result > (std::numeric_limits<std::uint64_t>::max() >> 8)) {
      return std::nullopt;
    }
    result = (result << 8) | byte;
  }
  return result;
}

std::optional<std::int64_t> ReadSigned(std::span<const std::uint8_t> value) {
  if (value.empty() || value.size() > sizeof(std::int64_t)) {
    return std::nullopt;
  }
  std::uint64_t bits = 0;
  for (const auto byte : value) {
    bits = (bits << 8) | byte;
  }
  if (value.size() < sizeof(std::int64_t) && (value.front() & 0x80) != 0) {
    bits |= ~std::uint64_t{0} << (value.size() * 8);
  }
  return static_cast<std::int64_t>(bits);
}

class BerWriter {
public:
  explicit BerWriter(std::span<std::uint8_t> output) : output_(output) {}

  bool AppendByte(std::uint8_t value) noexcept {
    if (offset_ >= output_.size()) {
      return false;
    }
    output_[offset_++] = value;
    return true;
  }

  bool AppendLength(std::size_t length) noexcept {
    if (length < 0x80) {
      return AppendByte(static_cast<std::uint8_t>(length));
    }
    std::array<std::uint8_t, sizeof(std::size_t)> bytes{};
    std::size_t count = 0;
    while (length != 0) {
      bytes[bytes.size() - ++count] = static_cast<std::uint8_t>(length);
      length >>= 8;
    }
    if (!AppendByte(static_cast<std::uint8_t>(0x80 | count))) {
      return false;
    }
    for (std::size_t index = bytes.size() - count; index < bytes.size();
         ++index) {
      if (!AppendByte(bytes[index])) {
        return false;
      }
    }
    return true;
  }

  bool AppendTlv(std::uint8_t tag,
                 std::span<const std::uint8_t> value) noexcept {
    if (!AppendByte(tag) || !AppendLength(value.size()) ||
        value.size() > output_.size() - offset_) {
      return false;
    }
    std::copy(value.begin(), value.end(), output_.begin() + offset_);
    offset_ += value.size();
    return true;
  }

  bool AppendString(std::uint8_t tag, std::string_view value) noexcept {
    return AppendTlv(
        tag, std::span<const std::uint8_t>(
                 reinterpret_cast<const std::uint8_t*>(value.data()),
                 value.size()));
  }

  bool AppendUnsigned(std::uint8_t tag, std::uint64_t value) noexcept {
    std::array<std::uint8_t, sizeof(value) + 1> bytes{};
    for (std::size_t index = bytes.size(); index != 1; --index) {
      bytes[index - 1] = static_cast<std::uint8_t>(value);
      value >>= 8;
    }
    std::size_t first = 0;
    while (first + 1 < bytes.size() && bytes[first] == 0 &&
           (bytes[first + 1] & 0x80) == 0) {
      ++first;
    }
    return AppendTlv(tag, std::span<const std::uint8_t>(
                              bytes.data() + first, bytes.size() - first));
  }

  bool AppendSigned(std::uint8_t tag, std::int64_t value) noexcept {
    std::array<std::uint8_t, sizeof(value)> bytes{};
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (std::size_t index = bytes.size(); index != 0; --index) {
      bytes[index - 1] = static_cast<std::uint8_t>(bits);
      bits >>= 8;
    }
    std::size_t first = 0;
    while (first + 1 < bytes.size()) {
      const bool negative = (bytes[first] & 0x80) != 0;
      const bool redundant =
          (negative && bytes[first + 1] == 0xff) ||
          (!negative && bytes[first + 1] == 0x00);
      if (!redundant) {
        break;
      }
      ++first;
    }
    if ((bytes[first] & 0x80) == 0 && value < 0) {
      if (first == 0) {
        return false;
      }
      --first;
      bytes[first] = 0xff;
    }
    if ((bytes[first] & 0x80) != 0 && value >= 0) {
      if (first == 0) {
        return false;
      }
      --first;
      bytes[first] = 0;
    }
    return AppendTlv(tag, std::span<const std::uint8_t>(
                              bytes.data() + first, bytes.size() - first));
  }

  std::size_t size() const noexcept { return offset_; }

private:
  std::span<std::uint8_t> output_;
  std::size_t offset_ = 0;
};

bool ReadBoolean(std::span<const std::uint8_t> value, bool* result) noexcept {
  // BER BOOLEAN必须使用一个内容字节；接受更长编码会把畸形报文误判为有效。
  if (result == nullptr || value.size() != 1) {
    return false;
  }
  *result = value.back() != 0;
  return true;
}

std::optional<std::array<std::uint8_t, 6>> ParseMac(std::string_view text) {
  std::array<std::uint8_t, 6> result{};
  std::size_t output = 0;
  std::uint8_t nibble = 0;
  bool haveHighNibble = false;
  bool lastWasSeparator = false;
  char separator = '\0';
  for (const auto character : text) {
    if (character == ':' || character == '-') {
      if (haveHighNibble || output == 0 || lastWasSeparator ||
          (separator != '\0' && separator != character)) {
        return std::nullopt;
      }
      separator = character;
      lastWasSeparator = true;
      continue;
    }
    std::uint8_t value = 0;
    if (character >= '0' && character <= '9') {
      value = static_cast<std::uint8_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      value = static_cast<std::uint8_t>(character - 'a' + 10);
    } else if (character >= 'A' && character <= 'F') {
      value = static_cast<std::uint8_t>(character - 'A' + 10);
    } else {
      return std::nullopt;
    }
    if (!haveHighNibble) {
      nibble = static_cast<std::uint8_t>(value << 4);
      haveHighNibble = true;
    } else {
      if (output >= result.size()) {
        return std::nullopt;
      }
      result[output++] = static_cast<std::uint8_t>(nibble | value);
      haveHighNibble = false;
    }
    lastWasSeparator = false;
  }
  return !haveHighNibble && !lastWasSeparator && output == result.size()
             ? std::optional<std::array<std::uint8_t, 6>>(result)
             : std::nullopt;
}

bool IsZeroMac(const std::array<std::uint8_t, 6>& mac) noexcept {
  return std::all_of(mac.begin(), mac.end(),
                     [](const auto value) { return value == 0; });
}

grpc::Status InvalidRealtimePlan(std::string_view kind,
                                 std::size_t index,
                                 std::string_view reason) {
  return grpc::Status(
      grpc::StatusCode::FAILED_PRECONDITION,
      std::format("IEC61850{}启动计划无效: 索引={}，{}", kind, index, reason));
}

grpc::Status ValidateRealtimeEndpoint(
    std::string_view kind, std::size_t streamIndex, std::size_t endpointIndex,
    IEC61850Proto::NetworkChannel channel, std::string_view interfaceName,
    std::string_view destinationMac, std::uint16_t appId, bool vlanTagged,
    std::uint16_t vlanId, std::uint8_t vlanPriority,
    std::unordered_set<int>* channels) {
  if (channel != IEC61850Proto::NETWORK_CHANNEL_A &&
      channel != IEC61850Proto::NETWORK_CHANNEL_B) {
    return InvalidRealtimePlan(kind, streamIndex,
                               std::format("端点{}通道必须是A或B", endpointIndex));
  }
  if (channels != nullptr &&
      !channels->emplace(static_cast<int>(channel)).second) {
    return InvalidRealtimePlan(
        kind, streamIndex,
        std::format("端点{}重复使用同一A/B通道", endpointIndex));
  }
  if (interfaceName.empty()) {
    return InvalidRealtimePlan(
        kind, streamIndex,
        std::format("端点{}缺少网卡名称", endpointIndex));
  }
  const auto mac = ParseMac(destinationMac);
  if (!mac.has_value() || IsZeroMac(*mac)) {
    return InvalidRealtimePlan(
        kind, streamIndex,
        std::format("端点{}目的MAC地址无效", endpointIndex));
  }
  if (appId == 0) {
    return InvalidRealtimePlan(
        kind, streamIndex,
        std::format("端点{}APPID不能为零", endpointIndex));
  }
  if (vlanId > 4095 || vlanPriority > 7) {
    return InvalidRealtimePlan(
        kind, streamIndex,
        std::format("端点{}VLAN参数超出范围", endpointIndex));
  }
  if (!vlanTagged && (vlanId != 0 || vlanPriority != 0)) {
    return InvalidRealtimePlan(
        kind, streamIndex,
        std::format("端点{}未标记VLAN时不能携带VLAN参数", endpointIndex));
  }
  return grpc::Status::OK;
}

grpc::Status ValidateRealtimePlans(const ProtocolIedPlan& plan) {
  if (plan.config.enable_goose()) {
    if (plan.gooseSubscriptions.empty() && plan.goosePublishers.empty()) {
      return InvalidRealtimePlan("GOOSE", 0,
                                 "至少需要一个订阅计划或本地发布计划");
    }
    const auto validateGooseRoute = [](std::string_view kind,
                                       std::size_t index,
                                       const auto& route) -> grpc::Status {
      if (route.controlRef.empty() || route.dataSetRef.empty() ||
          route.goId.empty() || route.configRevision == 0) {
        return InvalidRealtimePlan(
            kind, index, "控制块、DataSet、goID和ConfRev不能为空");
      }
      if (route.members.empty()) {
        return InvalidRealtimePlan(kind, index, "DataSet成员不能为空");
      }
      for (std::size_t memberIndex = 0; memberIndex < route.members.size();
           ++memberIndex) {
        const auto& member = route.members[memberIndex];
        if (member.dataRef.empty() ||
            member.fc == IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED) {
          return InvalidRealtimePlan(
              kind, index,
              std::format("成员{}缺少数据引用或功能约束", memberIndex));
        }
        if (member.valueType != IEC61850Proto::POINT_VALUE_TYPE_BOOL &&
            member.valueType != IEC61850Proto::POINT_VALUE_TYPE_INT64 &&
            member.valueType != IEC61850Proto::POINT_VALUE_TYPE_DOUBLE) {
          return InvalidRealtimePlan(
              kind, index,
              std::format("成员{}包含不支持的实时类型", memberIndex));
        }
        if (member.valueType == IEC61850Proto::POINT_VALUE_TYPE_DOUBLE &&
            member.encodedSize != 4 && member.encodedSize != 8) {
          return InvalidRealtimePlan(
              kind, index,
              std::format("成员{}的浮点编码宽度必须为4或8", memberIndex));
        }
        if (member.valueType != IEC61850Proto::POINT_VALUE_TYPE_DOUBLE &&
            member.encodedSize != 0) {
          return InvalidRealtimePlan(
              kind, index,
              std::format("成员{}的非浮点编码宽度必须为0", memberIndex));
        }
        if (member.qualityValue &&
            member.valueType != IEC61850Proto::POINT_VALUE_TYPE_INT64) {
          return InvalidRealtimePlan(
              kind, index,
              std::format("成员{}的Quality值必须使用INT64类型", memberIndex));
        }
      }
      if (route.endpoints.empty()) {
        return InvalidRealtimePlan(kind, index, "至少需要一个A/B二层端点");
      }
      std::unordered_set<int> channels;
      for (std::size_t endpointIndex = 0;
           endpointIndex < route.endpoints.size(); ++endpointIndex) {
        const auto& endpoint = route.endpoints[endpointIndex];
        const auto status = ValidateRealtimeEndpoint(
            kind, index, endpointIndex, endpoint.channel,
            endpoint.interfaceName, endpoint.destinationMac, endpoint.appId,
            endpoint.vlanTagged, endpoint.vlanId, endpoint.vlanPriority,
            &channels);
        if (!status.ok()) {
          return status;
        }
      }
      return grpc::Status::OK;
    };

    std::unordered_set<std::uint32_t> subscriptionIds;
    for (std::size_t index = 0; index < plan.gooseSubscriptions.size();
         ++index) {
      const auto& subscription = plan.gooseSubscriptions[index];
      if (subscription.subscriptionId == 0 ||
          !subscriptionIds.emplace(subscription.subscriptionId).second) {
        return InvalidRealtimePlan("GOOSE订阅", index,
                                   "订阅ID必须非零且唯一");
      }
      const auto status = validateGooseRoute("GOOSE订阅", index, subscription);
      if (!status.ok()) {
        return status;
      }
    }

    std::unordered_set<std::uint32_t> publisherIds;
    for (std::size_t index = 0; index < plan.goosePublishers.size(); ++index) {
      const auto& publisher = plan.goosePublishers[index];
      if (publisher.publisherId == 0 ||
          !publisherIds.emplace(publisher.publisherId).second) {
        return InvalidRealtimePlan("GOOSE发布", index,
                                   "发布端ID必须非零且唯一");
      }
      const auto status = validateGooseRoute("GOOSE发布", index, publisher);
      if (!status.ok()) {
        return status;
      }
    }
  }

  if (plan.config.enable_sv()) {
    std::unordered_set<std::uint32_t> streamIds;
    for (std::size_t index = 0; index < plan.svStreams.size(); ++index) {
      const auto& stream = plan.svStreams[index];
      if (stream.streamId == 0 || !streamIds.emplace(stream.streamId).second) {
        return InvalidRealtimePlan("SV采样流", index,
                                   "采样流ID必须非零且唯一");
      }
      if (stream.controlRef.empty() || stream.dataSetRef.empty() ||
          stream.svId.empty() || stream.configRevision == 0 ||
          stream.sampleRate == 0 || stream.nofAsdu == 0) {
        return InvalidRealtimePlan(
            "SV采样流", index,
            "控制块、DataSet、svID、ConfRev、采样率和ASDU数量不能为空");
      }
      if (stream.members.empty()) {
        return InvalidRealtimePlan("SV采样流", index,
                                   "DataSet成员不能为空");
      }
      for (std::size_t memberIndex = 0; memberIndex < stream.members.size();
           ++memberIndex) {
        const auto& member = stream.members[memberIndex];
        if (member.dataRef.empty() ||
            member.fc == IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED) {
          return InvalidRealtimePlan(
              "SV采样流", index,
              std::format("成员{}缺少数据引用或功能约束", memberIndex));
        }
        switch (member.encoding) {
          case ProtocolSvMemberEncoding::BOOLEAN:
            if (member.valueType != IEC61850Proto::POINT_VALUE_TYPE_BOOL ||
                member.encodedSize != 1) {
              return InvalidRealtimePlan(
                  "SV采样流", index,
                  std::format("成员{}的BOOLEAN编码宽度必须为1", memberIndex));
            }
            break;
          case ProtocolSvMemberEncoding::SIGNED_INTEGER:
          case ProtocolSvMemberEncoding::UNSIGNED_INTEGER:
            if (member.valueType != IEC61850Proto::POINT_VALUE_TYPE_INT64 ||
                member.encodedSize == 0 || member.encodedSize > 8) {
              return InvalidRealtimePlan(
                  "SV采样流", index,
                  std::format("成员{}的整数编码宽度必须在1至8字节", memberIndex));
            }
            break;
          case ProtocolSvMemberEncoding::FLOATING_POINT:
            if (member.valueType != IEC61850Proto::POINT_VALUE_TYPE_DOUBLE ||
                (member.encodedSize != 4 && member.encodedSize != 8)) {
              return InvalidRealtimePlan(
                  "SV采样流", index,
                  std::format("成员{}的浮点编码宽度必须为4或8", memberIndex));
            }
            break;
          default:
            return InvalidRealtimePlan(
                "SV采样流", index,
                std::format("成员{}包含未知采样编码", memberIndex));
        }
      }
      if (stream.endpoints.empty()) {
        return InvalidRealtimePlan("SV采样流", index,
                                   "至少需要一个A/B二层端点");
      }
      std::unordered_set<int> channels;
      for (std::size_t endpointIndex = 0;
           endpointIndex < stream.endpoints.size(); ++endpointIndex) {
        const auto& endpoint = stream.endpoints[endpointIndex];
        const auto status = ValidateRealtimeEndpoint(
            "SV采样流", index, endpointIndex, endpoint.channel,
            endpoint.interfaceName, endpoint.destinationMac, endpoint.appId,
            endpoint.vlanTagged, endpoint.vlanId, endpoint.vlanPriority,
            &channels);
        if (!status.ok()) {
          return status;
        }
      }
    }
  }
  return grpc::Status::OK;
}

std::int64_t NowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::array<std::uint8_t, 8> EncodeGooseUtcTime(
    std::int64_t timestampMs) noexcept {
  if (timestampMs <= 0) {
    timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  }
  if (timestampMs < 0) {
    timestampMs = 0;
  }
  const auto seconds = static_cast<std::uint64_t>(timestampMs / 1000);
  const auto remainderMs = static_cast<std::uint64_t>(timestampMs % 1000);
  const auto encodedSeconds = std::min<std::uint64_t>(
      seconds, std::numeric_limits<std::uint32_t>::max());
  const auto fraction = (remainderMs << 24) / 1000;
  std::array<std::uint8_t, 8> encoded{};
  encoded[0] = static_cast<std::uint8_t>(encodedSeconds >> 24);
  encoded[1] = static_cast<std::uint8_t>(encodedSeconds >> 16);
  encoded[2] = static_cast<std::uint8_t>(encodedSeconds >> 8);
  encoded[3] = static_cast<std::uint8_t>(encodedSeconds);
  encoded[4] = static_cast<std::uint8_t>(fraction >> 16);
  encoded[5] = static_cast<std::uint8_t>(fraction >> 8);
  encoded[6] = static_cast<std::uint8_t>(fraction);
  return encoded;
}

bool DecodeGooseFloatingPoint(std::span<const std::uint8_t> encoded,
                              std::uint8_t expectedSize,
                              double* result) noexcept {
  const std::uint8_t expectedFormatWidth =
      expectedSize == 4 ? std::uint8_t{0x08} : std::uint8_t{0x0b};
  if (result == nullptr || (expectedSize != 4 && expectedSize != 8) ||
      encoded.size() != static_cast<std::size_t>(expectedSize) + 1 ||
      encoded.front() != expectedFormatWidth) {
    return false;
  }
  if (expectedSize == 4) {
    std::uint32_t bits = 0;
    for (const auto byte : encoded.subspan(1)) {
      bits = (bits << 8) | byte;
    }
    float converted = 0.0F;
    std::memcpy(&converted, &bits, sizeof(converted));
    if (!std::isfinite(converted)) {
      return false;
    }
    *result = static_cast<double>(converted);
    return true;
  }
  std::uint64_t bits = 0;
  for (const auto byte : encoded.subspan(1)) {
    bits = (bits << 8) | byte;
  }
  double converted = 0.0;
  std::memcpy(&converted, &bits, sizeof(converted));
  if (!std::isfinite(converted)) {
    return false;
  }
  *result = converted;
  return true;
}

bool DecodeGooseValue(const BerTlv& tlv, ProtocolRealtimeValueType type,
                      std::uint8_t floatingSize, bool qualityValue,
                      ProtocolRealtimeValue* value) noexcept {
  if (value == nullptr) {
    return false;
  }
  if (qualityValue) {
    // IEC 61850 Quality是13位BIT STRING。BER内容由未使用位数和两个
    // 数据八位组组成，低3位必须为填充位，不能按普通整数直接解释。
    if (type != ProtocolRealtimeValueType::INTEGER || tlv.tag != 0x84 ||
        tlv.value.size() != 3 ||
        tlv.value.front() != 3) {
      return false;
    }
    const auto encoded = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(tlv.value[1]) << 8) | tlv.value[2]);
    if ((encoded & 0x0007u) != 0) {
      return false;
    }
    const auto bits = static_cast<std::uint16_t>(encoded >> 3);
    value->valueType = ProtocolRealtimeValueType::INTEGER;
    value->value.integerValue = bits;
    value->qualityBits = static_cast<std::uint32_t>(bits);
    return true;
  }
  switch (type) {
    case ProtocolRealtimeValueType::BOOLEAN:
      if (tlv.tag != 0x83 && tlv.tag != 0x01) {
        return false;
      }
      value->valueType = type;
      return ReadBoolean(tlv.value, &value->value.booleanValue);
    case ProtocolRealtimeValueType::INTEGER: {
      if (tlv.tag != 0x85 && tlv.tag != 0x86 && tlv.tag != 0x02) {
        return false;
      }
      value->valueType = type;
      if (tlv.tag == 0x86) {
        const auto integer = ReadUnsigned(tlv.value);
        if (!integer.has_value() ||
            *integer > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())) {
          return false;
        }
        value->value.integerValue = static_cast<std::int64_t>(*integer);
      } else {
        const auto integer = ReadSigned(tlv.value);
        if (!integer.has_value()) {
          return false;
        }
        value->value.integerValue = *integer;
      }
      return true;
    }
    case ProtocolRealtimeValueType::FLOATING: {
      if (tlv.tag != 0x87 ||
          !DecodeGooseFloatingPoint(tlv.value, floatingSize,
                                    &value->value.floatingValue)) {
        return false;
      }
      value->valueType = type;
      return true;
    }
  }
  return false;
}

bool DecodeGoosePayload(
    const RawEthernetFrameView& ethernet, const ProtocolGooseSubscriptionPlan& plan,
    IEC61850Proto::NetworkChannel channel,
    std::span<ProtocolRealtimeValue> values, ProtocolGooseFrameView* frame) {
  if (frame == nullptr || ethernet.payload.size() < 8 ||
      values.size() != plan.members.size()) {
    return false;
  }
  const auto pduLength = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(ethernet.payload[2]) << 8) |
      ethernet.payload[3]);
  if (pduLength < 8 || pduLength != ethernet.payload.size()) {
    return false;
  }
  const auto pdu = ethernet.payload.subspan(8);
  BerTlv outer;
  std::size_t pduOffset = 0;
  if (!ReadTlv(pdu, &pduOffset, &outer) || outer.tag != 0x61 ||
      pduOffset != pdu.size()) {
    return false;
  }
  BerTlv gocb;
  BerTlv ttl;
  BerTlv dataSet;
  BerTlv goId;
  BerTlv timestamp;
  BerTlv stNum;
  BerTlv sqNum;
  BerTlv test;
  BerTlv confRev;
  BerTlv ndsCom;
  BerTlv entryCount;
  BerTlv allData;
  std::size_t fieldOffset = 0;
  const auto readField = [&outer, &fieldOffset](std::uint8_t expectedTag,
                                                 BerTlv* field) {
    return field != nullptr && ReadTlv(outer.value, &fieldOffset, field) &&
           field->tag == expectedTag;
  };
  if (!readField(0x80, &gocb) || !readField(0x81, &ttl) ||
      !readField(0x82, &dataSet) || !readField(0x83, &goId) ||
      !readField(0x84, &timestamp) || !readField(0x85, &stNum) ||
      !readField(0x86, &sqNum) || !readField(0x87, &test) ||
      !readField(0x88, &confRev) || !readField(0x89, &ndsCom) ||
      !readField(0x8a, &entryCount) || !readField(0xab, &allData) ||
      fieldOffset != outer.value.size()) {
    return false;
  }
  const auto ttlValue = ReadGooseUnsigned(ttl.value);
  const auto stateNumber = ReadGooseUnsigned(stNum.value);
  const auto sequenceNumber = ReadGooseUnsigned(sqNum.value);
  const auto revision = ReadGooseUnsigned(confRev.value);
  const auto count = ReadGooseUnsigned(entryCount.value);
  bool simulation = false;
  bool needsCommissioning = false;
  if (!ttlValue.has_value() || !stateNumber.has_value() ||
      !sequenceNumber.has_value() || !revision.has_value() || !count.has_value() ||
      *ttlValue > std::numeric_limits<std::uint32_t>::max() ||
      *stateNumber > std::numeric_limits<std::uint32_t>::max() ||
      *sequenceNumber > std::numeric_limits<std::uint32_t>::max() ||
      !ReadBoolean(test.value, &simulation) ||
      !ReadBoolean(ndsCom.value, &needsCommissioning) ||
      timestamp.value.size() != 8 ||
      *count != plan.members.size()) {
    return false;
  }
  const auto receiveTimestampNs = NowNs();
  std::size_t offset = 0;
  BerTlv valueTlv;
  std::size_t valueIndex = 0;
  while (offset < allData.value.size()) {
    if (!ReadTlv(allData.value, &offset, &valueTlv) ||
        valueIndex >= values.size() ||
        !DecodeGooseValue(valueTlv, [&]() {
          switch (plan.members[valueIndex].valueType) {
            case IEC61850Proto::POINT_VALUE_TYPE_BOOL:
              return ProtocolRealtimeValueType::BOOLEAN;
            case IEC61850Proto::POINT_VALUE_TYPE_INT64:
              return ProtocolRealtimeValueType::INTEGER;
            case IEC61850Proto::POINT_VALUE_TYPE_DOUBLE:
              return ProtocolRealtimeValueType::FLOATING;
            default:
              return ProtocolRealtimeValueType::BOOLEAN;
          }
        }(), plan.members[valueIndex].encodedSize,
          plan.members[valueIndex].qualityValue, &values[valueIndex])) {
      return false;
    }
    values[valueIndex].timestampNs = receiveTimestampNs;
    ++valueIndex;
  }
  if (valueIndex != values.size()) {
    return false;
  }
  frame->subscriptionId = plan.subscriptionId;
  frame->gocbRef = std::string_view(
      reinterpret_cast<const char*>(gocb.value.data()), gocb.value.size());
  frame->dataSetRef = std::string_view(
      reinterpret_cast<const char*>(dataSet.value.data()), dataSet.value.size());
  frame->goId = std::string_view(
      reinterpret_cast<const char*>(goId.value.data()), goId.value.size());
  frame->channel = channel;
  frame->appId = ethernet.appId;
  frame->configRevision = *revision;
  frame->timeAllowedToLiveMs = static_cast<std::uint32_t>(*ttlValue);
  frame->stateNumber = static_cast<std::uint32_t>(*stateNumber);
  frame->sequenceNumber = static_cast<std::uint32_t>(*sequenceNumber);
  frame->simulation = simulation;
  frame->needsCommissioning = needsCommissioning;
  frame->receiveTimestampNs = receiveTimestampNs;
  frame->kernelTimestampNs = ethernet.kernelTimestampNs;
  frame->values = values;
  return true;
}

bool DecodeSvSample(std::span<const std::uint8_t> encoded,
                    const ProtocolSvMemberPlan& member,
                    ProtocolRealtimeValue* value) noexcept {
  if (value == nullptr || encoded.size() != member.encodedSize) {
    return false;
  }
  value->qualityBits = 0;
  switch (member.encoding) {
    case ProtocolSvMemberEncoding::BOOLEAN:
      value->valueType = ProtocolRealtimeValueType::BOOLEAN;
      if (encoded.front() != 0 && encoded.front() != 1) {
        return false;
      }
      value->value.booleanValue = encoded.back() != 0;
      return true;
    case ProtocolSvMemberEncoding::SIGNED_INTEGER: {
      const auto integer = ReadSigned(encoded);
      if (!integer.has_value()) {
        return false;
      }
      value->valueType = ProtocolRealtimeValueType::INTEGER;
      value->value.integerValue = *integer;
      return true;
    }
    case ProtocolSvMemberEncoding::UNSIGNED_INTEGER: {
      const auto integer = ReadUnsigned(encoded);
      if (!integer.has_value() ||
          *integer > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max())) {
        return false;
      }
      value->valueType = ProtocolRealtimeValueType::INTEGER;
      value->value.integerValue = static_cast<std::int64_t>(*integer);
      return true;
    }
    case ProtocolSvMemberEncoding::FLOATING_POINT:
      if (encoded.size() != 4 && encoded.size() != 8) {
        return false;
      }
      value->valueType = ProtocolRealtimeValueType::FLOATING;
      if (encoded.size() == 4) {
        std::uint32_t bits = 0;
        for (const auto byte : encoded) {
          bits = (bits << 8) | byte;
        }
        float converted = 0.0F;
        std::memcpy(&converted, &bits, sizeof(converted));
        value->value.floatingValue = converted;
      } else {
        std::uint64_t bits = 0;
        for (const auto byte : encoded) {
          bits = (bits << 8) | byte;
        }
        double converted = 0.0;
        std::memcpy(&converted, &bits, sizeof(converted));
        value->value.floatingValue = converted;
      }
      return std::isfinite(value->value.floatingValue);
  }
  return false;
}

bool AppendSvBerLength(std::vector<std::uint8_t>* output,
                       std::size_t length) {
  if (output == nullptr) {
    return false;
  }
  if (length < 0x80) {
    output->push_back(static_cast<std::uint8_t>(length));
    return true;
  }
  std::array<std::uint8_t, sizeof(std::size_t)> bytes{};
  std::size_t count = 0;
  while (length != 0) {
    bytes[bytes.size() - ++count] = static_cast<std::uint8_t>(length);
    length >>= 8;
  }
  if (count == 0 || count > 0x7f) {
    return false;
  }
  output->push_back(static_cast<std::uint8_t>(0x80 | count));
  output->insert(output->end(), bytes.end() - static_cast<std::ptrdiff_t>(count),
                 bytes.end());
  return true;
}

bool AppendSvBerTlv(std::vector<std::uint8_t>* output, std::uint8_t tag,
                    std::span<const std::uint8_t> value) {
  if (output == nullptr || value.size() >
                              std::numeric_limits<std::size_t>::max() - 2) {
    return false;
  }
  output->push_back(tag);
  if (!AppendSvBerLength(output, value.size())) {
    return false;
  }
  output->insert(output->end(), value.begin(), value.end());
  return true;
}

bool AppendSvBerString(std::vector<std::uint8_t>* output, std::uint8_t tag,
                       std::string_view value) {
  return AppendSvBerTlv(
      output, tag,
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

bool AppendSvBerUnsigned(std::vector<std::uint8_t>* output, std::uint8_t tag,
                         std::uint64_t value) {
  std::array<std::uint8_t, sizeof(value) + 1> bytes{};
  for (std::size_t index = bytes.size(); index != 1; --index) {
    bytes[index - 1] = static_cast<std::uint8_t>(value);
    value >>= 8;
  }
  std::size_t first = 0;
  while (first + 1 < bytes.size() && bytes[first] == 0 &&
         (bytes[first + 1] & 0x80) == 0) {
    ++first;
  }
  return AppendSvBerTlv(
      output, tag,
      std::span<const std::uint8_t>(bytes.data() + first,
                                    bytes.size() - first));
}

bool EncodeSvSample(const ProtocolSvMemberPlan& member,
                    const ProtocolRealtimeValue& value,
                    std::vector<std::uint8_t>* output) {
  if (output == nullptr || member.encodedSize == 0 ||
      member.encodedSize > sizeof(std::uint64_t)) {
    return false;
  }
  const auto expectedType = [&]() {
    switch (member.valueType) {
      case IEC61850Proto::POINT_VALUE_TYPE_BOOL:
        return ProtocolRealtimeValueType::BOOLEAN;
      case IEC61850Proto::POINT_VALUE_TYPE_INT64:
        return ProtocolRealtimeValueType::INTEGER;
      case IEC61850Proto::POINT_VALUE_TYPE_DOUBLE:
        return ProtocolRealtimeValueType::FLOATING;
      default:
        return ProtocolRealtimeValueType::BOOLEAN;
    }
  }();
  if (member.valueType == IEC61850Proto::POINT_VALUE_TYPE_UNSPECIFIED ||
      value.valueType != expectedType) {
    return false;
  }
  switch (member.encoding) {
    case ProtocolSvMemberEncoding::BOOLEAN:
      if (member.valueType != IEC61850Proto::POINT_VALUE_TYPE_BOOL ||
          member.encodedSize != 1) {
        return false;
      }
      output->push_back(static_cast<std::uint8_t>(
          value.value.booleanValue ? 0xff : 0x00));
      return true;
    case ProtocolSvMemberEncoding::SIGNED_INTEGER: {
      if (member.valueType != IEC61850Proto::POINT_VALUE_TYPE_INT64) {
        return false;
      }
      const auto width = static_cast<unsigned>(member.encodedSize);
      const auto number = value.value.integerValue;
      if (width < sizeof(std::int64_t)) {
        const auto bits = static_cast<unsigned>(width * 8);
        const auto minimum = -(std::int64_t{1} << (bits - 1));
        const auto maximum = (std::int64_t{1} << (bits - 1)) - 1;
        if (number < minimum || number > maximum) {
          return false;
        }
      }
      auto encoded = static_cast<std::uint64_t>(number);
      const auto offset = output->size();
      output->resize(offset + member.encodedSize);
      for (std::size_t index = member.encodedSize; index != 0; --index) {
        (*output)[offset + index - 1] = static_cast<std::uint8_t>(encoded);
        encoded >>= 8;
      }
      return true;
    }
    case ProtocolSvMemberEncoding::UNSIGNED_INTEGER: {
      if (member.valueType != IEC61850Proto::POINT_VALUE_TYPE_INT64 ||
          value.value.integerValue < 0) {
        return false;
      }
      const auto encoded = static_cast<std::uint64_t>(value.value.integerValue);
      if (member.encodedSize < sizeof(std::uint64_t) &&
          (encoded >> (member.encodedSize * 8)) != 0) {
        return false;
      }
      const auto offset = output->size();
      output->resize(offset + member.encodedSize);
      auto remaining = encoded;
      for (std::size_t index = member.encodedSize; index != 0; --index) {
        (*output)[offset + index - 1] = static_cast<std::uint8_t>(remaining);
        remaining >>= 8;
      }
      return true;
    }
    case ProtocolSvMemberEncoding::FLOATING_POINT: {
      if (member.valueType != IEC61850Proto::POINT_VALUE_TYPE_DOUBLE ||
          (member.encodedSize != 4 && member.encodedSize != 8) ||
          !std::isfinite(value.value.floatingValue)) {
        return false;
      }
      const auto offset = output->size();
      output->resize(offset + member.encodedSize);
      if (member.encodedSize == 4) {
        const auto converted = static_cast<float>(value.value.floatingValue);
        if (!std::isfinite(converted)) {
          output->resize(offset);
          return false;
        }
        std::uint32_t bits = 0;
        std::memcpy(&bits, &converted, sizeof(bits));
        for (std::size_t index = 4; index != 0; --index) {
          (*output)[offset + index - 1] = static_cast<std::uint8_t>(bits);
          bits >>= 8;
        }
      } else {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value.value.floatingValue, sizeof(bits));
        for (std::size_t index = 8; index != 0; --index) {
          (*output)[offset + index - 1] = static_cast<std::uint8_t>(bits);
          bits >>= 8;
        }
      }
      return true;
    }
  }
  return false;
}

bool DecodeSvAsdu(std::span<const std::uint8_t> asdu,
                  const ProtocolSvStreamPlan& plan,
                  std::span<ProtocolRealtimeValue> values,
                  ProtocolSvFrameView* frame,
                  const RawEthernetFrameView& ethernet,
                  IEC61850Proto::NetworkChannel channel,
                  std::uint32_t asduCount, std::uint32_t asduIndex,
                  std::int64_t receiveTimestampNs) {
  if (frame == nullptr || values.size() != plan.members.size()) {
    return false;
  }
  BerTlv svId;
  BerTlv smpCnt;
  BerTlv confRev;
  BerTlv smpSynch;
  BerTlv smpRate;
  BerTlv seqData;
  std::size_t fieldOffset = 0;
  BerTlv field;
  if (!ReadTlv(asdu, &fieldOffset, &field) || field.tag != 0x80) {
    return false;
  }
  svId = field;
  if (!ReadTlv(asdu, &fieldOffset, &field) || field.tag != 0x82) {
    return false;
  }
  smpCnt = field;
  if (!ReadTlv(asdu, &fieldOffset, &field) || field.tag != 0x83) {
    return false;
  }
  confRev = field;
  if (!ReadTlv(asdu, &fieldOffset, &field) || field.tag != 0x84) {
    return false;
  }
  smpSynch = field;
  bool hasRefrTm = false;
  bool hasSmpRate = false;
  for (;;) {
    if (!ReadTlv(asdu, &fieldOffset, &field)) {
      return false;
    }
    if (field.tag == 0x85) {
      if (hasRefrTm || hasSmpRate || field.value.size() != 8) {
        return false;
      }
      hasRefrTm = true;
      continue;
    }
    if (field.tag == 0x86) {
      if (hasSmpRate) {
        return false;
      }
      hasSmpRate = true;
      smpRate = field;
      continue;
    }
    if (field.tag != 0x87 || fieldOffset != asdu.size()) {
      return false;
    }
    seqData = field;
    break;
  }
  const auto sampleCount = ReadUnsigned(smpCnt.value);
  const auto revision = ReadUnsigned(confRev.value);
  const auto synchronization = ReadUnsigned(smpSynch.value);
  const auto sampleRate = hasSmpRate ? ReadUnsigned(smpRate.value)
                                     : std::optional<std::uint64_t>{};
  if (!sampleCount.has_value() || !revision.has_value() ||
      !synchronization.has_value() ||
      *sampleCount > std::numeric_limits<std::uint16_t>::max() ||
      *synchronization > 2 ||
      (hasSmpRate &&
       (!sampleRate.has_value() ||
        *sampleRate > std::numeric_limits<std::uint32_t>::max() ||
        (plan.sampleRate != 0 &&
         *sampleRate != static_cast<std::uint64_t>(plan.sampleRate)))) ||
      std::string_view(reinterpret_cast<const char*>(svId.value.data()),
                       svId.value.size()) != plan.svId ||
      *revision != plan.configRevision) {
    return false;
  }
  std::size_t expectedBytes = 0;
  for (const auto& member : plan.members) {
    if (member.encodedSize == 0 ||
        expectedBytes > std::numeric_limits<std::size_t>::max() -
                            member.encodedSize) {
      return false;
    }
    expectedBytes += member.encodedSize;
  }
  if (seqData.value.size() != expectedBytes) {
    return false;
  }
  std::size_t offset = 0;
  for (std::size_t index = 0; index < plan.members.size(); ++index) {
    const auto width = plan.members[index].encodedSize;
    if (!DecodeSvSample(seqData.value.subspan(offset, width),
                        plan.members[index], &values[index])) {
      return false;
    }
    values[index].timestampNs = receiveTimestampNs;
    offset += width;
  }
  frame->streamId = plan.streamId;
  frame->svId = std::string_view(
      reinterpret_cast<const char*>(svId.value.data()), svId.value.size());
  frame->channel = channel;
  frame->appId = ethernet.appId;
  frame->configRevision = *revision;
  frame->sampleCount = static_cast<std::uint16_t>(*sampleCount);
  frame->sampleSynchronization = static_cast<std::uint8_t>(*synchronization);
  frame->asduCount = asduCount;
  frame->asduIndex = asduIndex;
  frame->receiveTimestampNs = receiveTimestampNs;
  frame->kernelTimestampNs = ethernet.kernelTimestampNs;
  frame->values = values;
  return true;
}

std::size_t CountAsdu(std::span<const std::uint8_t> input) noexcept {
  std::size_t offset = 0;
  std::size_t count = 0;
  BerTlv tlv;
  while (offset < input.size()) {
    if (!ReadTlv(input, &offset, &tlv)) {
      return 0;
    }
    if (tlv.tag == 0x30) {
      ++count;
    }
  }
  return count;
}

void InvokeGooseCallback(const ProtocolEventCallbacks& callbacks,
                         ProtocolGooseFrameView frame) noexcept {
  if (!callbacks.onGooseFrame) {
    return;
  }
  try {
    callbacks.onGooseFrame(frame);
  } catch (const std::exception& exception) {
    LOG_ERROR("IEC61850处理GOOSE接收回调时发生异常: 订阅={}, 异常信息={}",
              frame.subscriptionId, exception.what());
  } catch (...) {
    LOG_ERROR("IEC61850处理GOOSE接收回调时发生未知异常: 订阅={}",
              frame.subscriptionId);
  }
}

bool EncodeGooseValue(BerWriter* writer, const ProtocolRealtimeValue& value,
                      const ProtocolGooseMemberPlan& member) noexcept {
  if (writer == nullptr) {
    return false;
  }
  const auto expectedType = [&]() {
    switch (member.valueType) {
      case IEC61850Proto::POINT_VALUE_TYPE_BOOL:
        return ProtocolRealtimeValueType::BOOLEAN;
      case IEC61850Proto::POINT_VALUE_TYPE_INT64:
        return ProtocolRealtimeValueType::INTEGER;
      case IEC61850Proto::POINT_VALUE_TYPE_DOUBLE:
        return ProtocolRealtimeValueType::FLOATING;
      default:
        return ProtocolRealtimeValueType::BOOLEAN;
    }
  }();
  if (member.valueType == IEC61850Proto::POINT_VALUE_TYPE_UNSPECIFIED ||
      value.valueType != expectedType) {
    return false;
  }
  if (member.qualityValue) {
    if (value.valueType != ProtocolRealtimeValueType::INTEGER ||
        value.value.integerValue < 0 || value.value.integerValue > 0x1fff) {
      return false;
    }
    // Quality的13个有效位位于BIT STRING的高13位，末尾3位为未使用位。
    const auto encoded = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(value.value.integerValue) << 3);
    const std::array<std::uint8_t, 3> quality{
        3, static_cast<std::uint8_t>(encoded >> 8),
        static_cast<std::uint8_t>(encoded)};
    return writer->AppendTlv(0x84, quality);
  }
  switch (value.valueType) {
    case ProtocolRealtimeValueType::BOOLEAN: {
      const std::array<std::uint8_t, 1> encoded{
          static_cast<std::uint8_t>(value.value.booleanValue ? 0xff : 0x00)};
      return writer->AppendTlv(0x83, encoded);
    }
    case ProtocolRealtimeValueType::INTEGER:
      return writer->AppendSigned(0x85, value.value.integerValue);
    case ProtocolRealtimeValueType::FLOATING: {
      if (!std::isfinite(value.value.floatingValue)) {
        return false;
      }
      if (member.encodedSize != 4 && member.encodedSize != 8) {
        return false;
      }
      std::array<std::uint8_t, 9> encoded{};
      encoded[0] = member.encodedSize == 4 ? 0x08 : 0x0b;
      if (member.encodedSize == 4) {
        const float converted =
            static_cast<float>(value.value.floatingValue);
        if (!std::isfinite(converted)) {
          return false;
        }
        std::uint32_t bits = 0;
        std::memcpy(&bits, &converted, sizeof(bits));
        for (std::size_t index = 4; index != 0; --index) {
          encoded[index] = static_cast<std::uint8_t>(bits);
          bits >>= 8;
        }
      } else {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value.value.floatingValue, sizeof(bits));
        for (std::size_t index = 8; index != 0; --index) {
          encoded[index] = static_cast<std::uint8_t>(bits);
          bits >>= 8;
        }
      }
      return writer->AppendTlv(
          0x87, std::span<const std::uint8_t>(encoded.data(),
                                               member.encodedSize + 1));
    }
  }
  return false;
}

void InvokeSvCallback(const ProtocolEventCallbacks& callbacks,
                      ProtocolSvFrameView frame) noexcept {
  if (!callbacks.onSvFrame) {
    return;
  }
  try {
    callbacks.onSvFrame(frame);
  } catch (const std::exception& exception) {
    LOG_ERROR("IEC61850处理SV接收回调时发生异常: 采样流={}, 异常信息={}",
              frame.streamId, exception.what());
  } catch (...) {
    LOG_ERROR("IEC61850处理SV接收回调时发生未知异常: 采样流={}",
              frame.streamId);
  }
}

}  // namespace

bool DecodeGoosePayload(std::span<const std::uint8_t> payload,
                        const ProtocolGooseSubscriptionPlan& plan,
                        IEC61850Proto::NetworkChannel channel,
                        std::span<ProtocolRealtimeValue> values,
                        ProtocolGooseFrameView* frame) {
  RawEthernetFrameView ethernet;
  ethernet.payload = payload;
  ethernet.appId = payload.size() >= 2
                      ? static_cast<std::uint16_t>(
                            (static_cast<std::uint16_t>(payload[0]) << 8) |
                            payload[1])
                      : 0;
  return DecodeGoosePayload(ethernet, plan, channel, values, frame);
}

bool DecodeSvPayload(std::span<const std::uint8_t> payload,
                     const ProtocolSvStreamPlan& plan,
                     IEC61850Proto::NetworkChannel channel,
                     std::span<ProtocolRealtimeValue> values,
                     std::span<ProtocolSvFrameView> frames,
                     std::size_t* frameCount) {
  if (frameCount == nullptr) {
    return false;
  }
  *frameCount = 0;
  if (payload.size() < 8 || plan.nofAsdu == 0 || plan.members.empty() ||
      frames.size() < plan.nofAsdu ||
      plan.nofAsdu > std::numeric_limits<std::size_t>::max() /
                           plan.members.size() ||
      values.size() < plan.nofAsdu * plan.members.size()) {
    return false;
  }
  const auto pduLength = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(payload[2]) << 8) | payload[3]);
  if (pduLength < 8 || pduLength != payload.size()) {
    return false;
  }
  RawEthernetFrameView ethernet;
  ethernet.payload = payload;
  ethernet.appId = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(payload[0]) << 8) | payload[1]);
  BerTlv outer;
  const auto savPdu = payload.subspan(8);
  std::size_t savPduOffset = 0;
  if (!ReadTlv(savPdu, &savPduOffset, &outer) || outer.tag != 0x60 ||
      savPduOffset != savPdu.size()) {
    return false;
  }
  BerTlv noAsdu;
  BerTlv container;
  std::size_t outerOffset = 0;
  if (!ReadTlv(outer.value, &outerOffset, &noAsdu) || noAsdu.tag != 0x80 ||
      !ReadTlv(outer.value, &outerOffset, &container) ||
      container.tag != 0xa2 || outerOffset != outer.value.size()) {
    return false;
  }
  const auto declaredAsduCount = ReadUnsigned(noAsdu.value);
  if (!declaredAsduCount.has_value() ||
      *declaredAsduCount > std::numeric_limits<std::uint32_t>::max() ||
      *declaredAsduCount != plan.nofAsdu) {
    return false;
  }
  const auto asduBytes = container.value;
  const auto count = CountAsdu(asduBytes);
  if (count == 0 || count != static_cast<std::size_t>(*declaredAsduCount)) {
    return false;
  }
  const auto receiveTimestampNs = NowNs();
  std::size_t offset = 0;
  std::size_t decoded = 0;
  while (offset < asduBytes.size()) {
    BerTlv asdu;
    if (!ReadTlv(asduBytes, &offset, &asdu) || asdu.tag != 0x30 ||
        decoded >= count ||
        !DecodeSvAsdu(
            asdu.value, plan,
            values.subspan(decoded * plan.members.size(), plan.members.size()),
            &frames[decoded], ethernet, channel,
            static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(decoded),
            receiveTimestampNs)) {
      *frameCount = 0;
      return false;
    }
    ++decoded;
  }
  if (decoded != count) {
    *frameCount = 0;
    return false;
  }
  *frameCount = decoded;
  return true;
}

bool EncodeSvPayload(const SvPublishRequest& request, std::uint16_t appId,
                     std::span<std::uint8_t> output,
                     std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return false;
  }
  *outputSize = 0;
  if (output.size() < 8 || appId == 0 || request.svId.empty() ||
      request.configRevision == 0 || request.sampleRate == 0 ||
      request.sampleSynchronization > 2 || request.sampleCounts.empty() ||
      request.sampleCounts.size() > std::numeric_limits<std::uint32_t>::max() ||
      request.members.empty() ||
      request.sampleCounts.size() >
          std::numeric_limits<std::size_t>::max() / request.members.size() ||
      request.values.size() !=
          request.sampleCounts.size() * request.members.size()) {
    return false;
  }

  const auto timestamp = EncodeGooseUtcTime(request.referenceTimeMs);
  std::vector<std::uint8_t> asduContainer;
  for (std::size_t asduIndex = 0; asduIndex < request.sampleCounts.size();
       ++asduIndex) {
    std::vector<std::uint8_t> sequenceData;
    sequenceData.reserve(request.members.size() * 8);
    const auto valueOffset = asduIndex * request.members.size();
    for (std::size_t memberIndex = 0; memberIndex < request.members.size();
         ++memberIndex) {
      if (!EncodeSvSample(request.members[memberIndex],
                          request.values[valueOffset + memberIndex],
                          &sequenceData)) {
        return false;
      }
    }
    std::vector<std::uint8_t> asdu;
    if (!AppendSvBerString(&asdu, 0x80, request.svId) ||
        !AppendSvBerUnsigned(&asdu, 0x82,
                             request.sampleCounts[asduIndex]) ||
        !AppendSvBerUnsigned(&asdu, 0x83, request.configRevision) ||
        !AppendSvBerUnsigned(&asdu, 0x84, request.sampleSynchronization) ||
        !AppendSvBerTlv(&asdu, 0x85, timestamp) ||
        !AppendSvBerUnsigned(&asdu, 0x86, request.sampleRate) ||
        !AppendSvBerTlv(&asdu, 0x87, sequenceData)) {
      return false;
    }
    if (!AppendSvBerTlv(&asduContainer, 0x30, asdu)) {
      return false;
    }
  }

  std::vector<std::uint8_t> outerContent;
  if (!AppendSvBerUnsigned(&outerContent, 0x80,
                           static_cast<std::uint32_t>(
                               request.sampleCounts.size())) ||
      !AppendSvBerTlv(&outerContent, 0xa2, asduContainer)) {
    return false;
  }
  std::vector<std::uint8_t> savPdu;
  if (!AppendSvBerTlv(&savPdu, 0x60, outerContent) ||
      savPdu.size() > std::numeric_limits<std::uint16_t>::max() - 8 ||
      output.size() < 8 + savPdu.size()) {
    return false;
  }
  const auto totalSize = 8 + savPdu.size();
  output[0] = static_cast<std::uint8_t>(appId >> 8);
  output[1] = static_cast<std::uint8_t>(appId);
  output[2] = static_cast<std::uint8_t>(totalSize >> 8);
  output[3] = static_cast<std::uint8_t>(totalSize);
  output[4] = 0;
  output[5] = 0;
  output[6] = 0;
  output[7] = 0;
  std::copy(savPdu.begin(), savPdu.end(), output.begin() + 8);
  *outputSize = totalSize;
  return true;
}

bool EncodeGoosePayload(const GoosePublishRequest& request,
                        std::uint32_t stateNumber,
                        std::uint32_t sequenceNumber,
                        std::uint16_t appId,
                        std::span<std::uint8_t> output,
                        std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return false;
  }
  *outputSize = 0;
  if (output.size() < 8 || appId == 0 || request.gocbRef.empty() ||
      request.dataSetRef.empty() || request.goId.empty() ||
      request.timeAllowedToLiveMs == 0 || request.configRevision == 0 ||
      request.values.empty() || request.members.size() != request.values.size() ||
      stateNumber == 0 ||
      request.values.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  std::array<std::uint8_t, 2048> allData{};
  BerWriter allDataWriter(allData);
  for (std::size_t index = 0; index < request.values.size(); ++index) {
    if (!EncodeGooseValue(&allDataWriter, request.values[index],
                          request.members[index])) {
      return false;
    }
  }
  std::array<std::uint8_t, 2048> content{};
  BerWriter contentWriter(content);
  const std::array<std::uint8_t, 1> simulation{
      static_cast<std::uint8_t>(request.simulation ? 0xff : 0x00)};
  const std::array<std::uint8_t, 1> needsCommissioning{
      static_cast<std::uint8_t>(request.needsCommissioning ? 0xff : 0x00)};
  const auto timestamp = EncodeGooseUtcTime(request.timestampMs);
  if (!contentWriter.AppendString(0x80, request.gocbRef) ||
      !contentWriter.AppendUnsigned(0x81, request.timeAllowedToLiveMs) ||
      !contentWriter.AppendString(0x82, request.dataSetRef) ||
      !contentWriter.AppendString(0x83, request.goId) ||
      !contentWriter.AppendTlv(0x84, timestamp) ||
      !contentWriter.AppendUnsigned(0x85, stateNumber) ||
      !contentWriter.AppendUnsigned(0x86, sequenceNumber) ||
      !contentWriter.AppendTlv(0x87, simulation) ||
      !contentWriter.AppendUnsigned(0x88, request.configRevision) ||
      !contentWriter.AppendTlv(0x89, needsCommissioning) ||
      !contentWriter.AppendUnsigned(0x8a, request.values.size()) ||
      !contentWriter.AppendTlv(
          0xab, std::span<const std::uint8_t>(allData.data(),
                                              allDataWriter.size()))) {
    return false;
  }
  BerWriter pduWriter(output.subspan(8));
  if (!pduWriter.AppendTlv(
          0x61, std::span<const std::uint8_t>(content.data(),
                                              contentWriter.size()))) {
    return false;
  }
  const auto totalSize = 8 + pduWriter.size();
  if (totalSize > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  output[0] = static_cast<std::uint8_t>(appId >> 8);
  output[1] = static_cast<std::uint8_t>(appId);
  output[2] = static_cast<std::uint8_t>(totalSize >> 8);
  output[3] = static_cast<std::uint8_t>(totalSize);
  output[4] = 0;
  output[5] = 0;
  output[6] = 0;
  output[7] = 0;
  *outputSize = totalSize;
  return true;
}

class RawProtocolStackAdapter final : public ProtocolStackAdapter {
  struct GoosePublishRoute {
    std::uint32_t publisherId = 0;
    std::uint32_t subscriptionId = 0;
    std::vector<std::unique_ptr<GoosePublisher>> publishers;
    std::uint32_t stateNumber = 0;
    std::uint32_t sequenceNumber = 0;
    bool hasValues = false;
    bool retransmitActive = false;
    std::uint32_t retransmitIntervalMs = 0;
    std::chrono::steady_clock::time_point nextRetransmitAt{};
  };

  struct Runtime {
    ThreadRuntimePolicy runtimePolicy;
    std::vector<std::jthread> workers;
    std::unique_ptr<MmsSessionWorker> mmsWorker;
    std::vector<GoosePublishRoute> goosePublishRoutes;
    std::mutex publishMutex;
    std::condition_variable publishCondition;
    std::uint64_t publishRevision = 0;
    bool stopping = false;
    std::jthread gooseRetransmitWorker;

    grpc::Status StartGooseRetransmit() {
      std::jthread worker;
      const auto status = StartThreadWithRuntimePolicy(
          &worker, runtimePolicy,
          [this](std::stop_token stopToken) {
            RunGooseRetransmit(std::move(stopToken));
          });
      if (!status.ok()) {
        return status;
      }
      gooseRetransmitWorker = std::move(worker);
      return grpc::Status::OK;
    }

    void RunGooseRetransmit(std::stop_token stopToken) noexcept {
      try {
        std::unique_lock lock(publishMutex);
        while (!stopToken.stop_requested() && !stopping) {
          std::optional<std::chrono::steady_clock::time_point> nextDue;
          for (const auto& route : goosePublishRoutes) {
            if (!route.retransmitActive) {
              continue;
            }
            if (!nextDue.has_value() || route.nextRetransmitAt < *nextDue) {
              nextDue = route.nextRetransmitAt;
            }
          }
          if (!nextDue.has_value()) {
            const auto revision = publishRevision;
            publishCondition.wait(lock, [&] {
              return stopToken.stop_requested() || stopping ||
                     revision != publishRevision;
            });
            continue;
          }
          const auto revision = publishRevision;
          if (publishCondition.wait_until(lock, *nextDue, [&] {
                return stopToken.stop_requested() || stopping ||
                       revision != publishRevision;
              })) {
            continue;
          }
          const auto now = std::chrono::steady_clock::now();
          for (auto& route : goosePublishRoutes) {
            if (!route.retransmitActive || now < route.nextRetransmitAt) {
              continue;
            }
            if (route.sequenceNumber ==
                std::numeric_limits<std::uint32_t>::max()) {
              route.retransmitActive = false;
              LOG_ERROR("IEC61850 GOOSE重发序号耗尽: 发布端={}, 订阅={}",
                        route.publisherId, route.subscriptionId);
              continue;
            }
            bool sent = true;
            for (const auto& publisher : route.publishers) {
              const auto status = publisher->Retransmit();
              if (!status.ok()) {
                sent = false;
                LOG_WARNING(
                    "IEC61850 GOOSE自动重发失败: 发布端={}, 订阅={}, 原因={}",
                    route.publisherId, route.subscriptionId,
                    status.error_message());
              }
            }
            if (!sent) {
              route.retransmitActive = false;
              continue;
            }
            ++route.sequenceNumber;
            route.retransmitIntervalMs = NextGooseRetransmitIntervalMs(
                route.retransmitIntervalMs,
                kGooseDefaultMaxRetransmitIntervalMs);
            route.nextRetransmitAt =
                now + std::chrono::milliseconds(route.retransmitIntervalMs);
            LOG_DEBUG(
                "IEC61850 GOOSE自动重发: 发布端={}, 订阅={}, stNum={}, sqNum={}, 间隔毫秒={}",
                route.publisherId, route.subscriptionId, route.stateNumber,
                route.sequenceNumber, route.retransmitIntervalMs);
          }
        }
      } catch (const std::exception& exception) {
        LOG_ERROR("IEC61850 GOOSE重发线程发生异常: {}", exception.what());
      } catch (...) {
        LOG_ERROR("IEC61850 GOOSE重发线程发生未知异常");
      }
    }

    ~Runtime() { Stop(); }

    void Stop() noexcept {
      if (mmsWorker != nullptr) {
        mmsWorker->Stop();
      }
      for (auto& worker : workers) {
        worker.request_stop();
      }
      {
        std::lock_guard lock(publishMutex);
        stopping = true;
        ++publishRevision;
      }
      gooseRetransmitWorker.request_stop();
      publishCondition.notify_all();
      workers.clear();
      gooseRetransmitWorker = std::jthread{};
      for (auto& route : goosePublishRoutes) {
        for (auto& publisher : route.publishers) {
          if (publisher != nullptr) {
            publisher->Close();
          }
        }
      }
      goosePublishRoutes.clear();
      mmsWorker.reset();
    }
  };

public:
  ~RawProtocolStackAdapter() override { StopIed(std::string_view{}); }

  grpc::Status StartIed(ProtocolIedPlan plan,
                        ProtocolEventCallbacks callbacks) override {
    std::unique_lock lock(mutex_);
    const auto connName = plan.config.conn_name();
    if (connName.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "IEC61850原始二层会话连接名不能为空");
    }
    if (runtimes_.contains(connName)) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850原始二层协议会话已经启动");
    }
    if (plan.config.enable_mms() && !callbacks.onMmsConnection) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          "启用MMS时必须存在连接状态回调");
    }
    if (plan.config.enable_goose() && plan.gooseSubscriptions.empty() &&
        plan.goosePublishers.empty()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "启用GOOSE时必须存在订阅计划或本地发布计划");
    }
    if (plan.config.enable_goose() && !plan.gooseSubscriptions.empty() &&
        !callbacks.onGooseFrame) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "启用GOOSE订阅时必须存在接收回调");
    }
    if (plan.config.enable_sv() &&
        (plan.svStreams.empty() || !callbacks.onSvFrame)) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "启用SV时必须存在采样流计划和接收回调");
    }
    const auto validation = ValidateRealtimePlans(plan);
    if (!validation.ok()) {
      LOG_ERROR("IEC61850原始二层启动计划校验失败: {}",
                validation.error_message());
      return validation;
    }
    const auto runtimePolicyStatus =
        ValidateThreadRuntimePolicy(plan.realtimePolicy);
    if (!runtimePolicyStatus.ok()) {
      LOG_ERROR("IEC61850原始二层线程运行策略校验失败: IED={}, 原因={}",
                connName, runtimePolicyStatus.error_message());
      return runtimePolicyStatus;
    }
    auto runtime = std::make_shared<Runtime>();
    runtime->runtimePolicy = plan.realtimePolicy;
    const auto abortStart = [&lock, &runtime](grpc::Status status) {
      lock.unlock();
      runtime->Stop();
      return status;
    };
    if (plan.config.enable_mms()) {
      runtime->mmsWorker = std::make_unique<MmsSessionWorker>(
          plan, plan.networkBindings, callbacks);
      const auto status = runtime->mmsWorker->Start();
      if (!status.ok()) {
        return abortStart(status);
      }
    }
    if (plan.config.enable_goose()) {
      for (const auto& publisherPlan : plan.goosePublishers) {
        auto& route = runtime->goosePublishRoutes.emplace_back();
        route.publisherId = publisherPlan.publisherId;
        // 兼容旧调用方；新调用优先使用publisherId。
        route.subscriptionId = publisherPlan.publisherId;
        for (const auto& endpoint : publisherPlan.endpoints) {
          GoosePublisherConfig publisherConfig;
          publisherConfig.endpoint = endpoint;
          publisherConfig.gocbRef = publisherPlan.controlRef;
          publisherConfig.dataSetRef = publisherPlan.dataSetRef;
          publisherConfig.goId = publisherPlan.goId;
          publisherConfig.configRevision = publisherPlan.configRevision;
          publisherConfig.timeAllowedToLiveMs = 1000;
          publisherConfig.memberCount = publisherPlan.members.size();
          publisherConfig.members = publisherPlan.members;
          auto publisher = std::make_unique<GoosePublisher>(
              std::move(publisherConfig));
          const auto status = publisher->Open();
          if (!status.ok()) {
            return abortStart(status);
          }
          route.publishers.emplace_back(std::move(publisher));
        }
      }
      // 订阅计划只描述远端发布者和接收过滤条件，不能推断为本地发布器；
      // 本地发送端点必须来自当前IED自己的GSEControl发布计划。
    }
    if (plan.config.enable_goose()) {
      for (const auto& subscription : plan.gooseSubscriptions) {
        for (const auto& endpoint : subscription.endpoints) {
          auto mac = ParseMac(endpoint.destinationMac);
          if (!mac.has_value()) {
            return abortStart(grpc::Status(
                grpc::StatusCode::FAILED_PRECONDITION,
                "GOOSE通信地址MAC格式无效"));
          }
          auto workerSocket = std::make_shared<RawEthernetSocket>();
          RawEthernetFilter filter;
          filter.destinationMac = *mac;
          filter.etherType = kGooseEtherType;
          filter.appId = endpoint.appId;
          filter.vlanTagged = endpoint.vlanTagged;
          filter.vlanId = endpoint.vlanId;
          if (endpoint.vlanTagged) {
            filter.vlanPriority = endpoint.vlanPriority;
          }
          const auto status = workerSocket->Open(
              endpoint.interfaceName, filter,
              kRawEthernetReceiveBufferBytes);
          if (!status.ok()) {
            return abortStart(status);
          }
          auto values = std::make_shared<std::vector<ProtocolRealtimeValue>>(
              subscription.members.size());
          std::jthread worker;
          const auto threadStatus = StartThreadWithRuntimePolicy(
              &worker, runtime->runtimePolicy,
              [workerSocket, values, subscription, endpoint,
               callbacks](std::stop_token stopToken) mutable {
                std::array<std::uint8_t, kEthernetFrameCapacity> storage{};
                while (!stopToken.stop_requested()) {
                  RawEthernetFrameView ethernet;
                  const auto status = workerSocket->Receive(storage, &ethernet);
                  if (!status.ok()) {
                    if (status.error_code() != grpc::StatusCode::RESOURCE_EXHAUSTED) {
                      LOG_WARNING("IEC61850接收GOOSE报文失败: {}",
                                  status.error_message());
                    }
                  } else if (!ethernet.payload.empty() && callbacks.onGooseFrame) {
                    ProtocolGooseFrameView frame;
                    if (DecodeGoosePayload(
                            ethernet, subscription, endpoint.channel,
                            std::span<ProtocolRealtimeValue>(
                                values->data(), values->size()),
                            &frame)) {
                      InvokeGooseCallback(callbacks, frame);
                    }
                  }
                  if (ethernet.payload.empty()) {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(100));
                  }
                }
                workerSocket->Close();
              });
          if (!threadStatus.ok()) {
            workerSocket->Close();
            return abortStart(threadStatus);
          }
          runtime->workers.emplace_back(std::move(worker));
        }
      }
    }
    if (plan.config.enable_sv()) {
      for (const auto& stream : plan.svStreams) {
        for (const auto& endpoint : stream.endpoints) {
          auto mac = ParseMac(endpoint.destinationMac);
          if (!mac.has_value()) {
            return abortStart(grpc::Status(
                grpc::StatusCode::FAILED_PRECONDITION,
                "SV通信地址MAC格式无效"));
          }
          auto workerSocket = std::make_shared<RawEthernetSocket>();
          RawEthernetFilter filter;
          filter.destinationMac = *mac;
          filter.etherType = kSvEtherType;
          filter.appId = endpoint.appId;
          filter.vlanTagged = endpoint.vlanTagged;
          filter.vlanId = endpoint.vlanId;
          if (endpoint.vlanTagged) {
            filter.vlanPriority = endpoint.vlanPriority;
          }
          const auto status = workerSocket->Open(
              endpoint.interfaceName, filter,
              kRawEthernetReceiveBufferBytes);
          if (!status.ok()) {
            return abortStart(status);
          }
          if (stream.members.empty() ||
              stream.nofAsdu > std::numeric_limits<std::size_t>::max() /
                                   stream.members.size()) {
            workerSocket->Close();
            return abortStart(grpc::Status(
                grpc::StatusCode::RESOURCE_EXHAUSTED,
                "SV采样流成员数量或ASDU数量超过下位机内存边界"));
          }
          auto values = std::make_shared<std::vector<ProtocolRealtimeValue>>(
              stream.members.size() * stream.nofAsdu);
          auto frames = std::make_shared<std::vector<ProtocolSvFrameView>>(
              stream.nofAsdu);
          std::jthread worker;
          const auto threadStatus = StartThreadWithRuntimePolicy(
              &worker, runtime->runtimePolicy,
              [workerSocket, values, frames, stream, endpoint,
               callbacks](std::stop_token stopToken) mutable {
                std::array<std::uint8_t, kEthernetFrameCapacity> storage{};
                while (!stopToken.stop_requested()) {
                  RawEthernetFrameView ethernet;
                  const auto status = workerSocket->Receive(storage, &ethernet);
                  if (!status.ok()) {
                    if (status.error_code() != grpc::StatusCode::RESOURCE_EXHAUSTED) {
                      LOG_WARNING("IEC61850接收SV报文失败: {}",
                                  status.error_message());
                    }
                  } else if (!ethernet.payload.empty() &&
                             callbacks.onSvFrame) {
                    std::size_t frameCount = 0;
                    if (DecodeSvPayload(
                            ethernet.payload, stream, endpoint.channel,
                            std::span<ProtocolRealtimeValue>(
                                values->data(), values->size()),
                            std::span<ProtocolSvFrameView>(frames->data(),
                                                           frames->size()),
                            &frameCount)) {
                      for (std::size_t index = 0; index < frameCount; ++index) {
                        InvokeSvCallback(callbacks, (*frames)[index]);
                      }
                    }
                  }
                  if (ethernet.payload.empty()) {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(50));
                  }
                }
                workerSocket->Close();
              });
          if (!threadStatus.ok()) {
            workerSocket->Close();
            return abortStart(threadStatus);
          }
          runtime->workers.emplace_back(std::move(worker));
        }
      }
    }
    if (plan.config.enable_goose() &&
        !runtime->goosePublishRoutes.empty()) {
      const auto status = runtime->StartGooseRetransmit();
      if (!status.ok()) {
        return abortStart(status);
      }
    }
    runtimes_.emplace(connName, std::move(runtime));
    LOG_INFO("IEC61850自研IEC61850协议会话已启动: IED={}, MMS={}, GOOSE={}, SV={}",
             plan.config.conn_name(), plan.config.enable_mms() ? "启用" : "停用",
             plan.config.enable_goose() ? "启用" : "停用",
             plan.config.enable_sv() ? "启用" : "停用");
    return grpc::Status::OK;
  }

  grpc::Status PublishGoose(
      std::string_view connName,
      const ProtocolGoosePublishCommand& command) override {
    std::lock_guard lock(mutex_);
    const auto runtimeIt = runtimes_.find(std::string(connName));
    if (runtimeIt == runtimes_.end()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 GOOSE发布会话尚未启动");
    }
    auto& runtime = *runtimeIt->second;
    std::lock_guard publishLock(runtime.publishMutex);
    if (runtime.stopping) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 GOOSE发布会话正在停止");
    }
    const auto routeIt = std::find_if(
        runtime.goosePublishRoutes.begin(), runtime.goosePublishRoutes.end(),
        [&command](const auto& route) {
          if (command.publisherId != 0) {
            return route.publisherId == command.publisherId;
          }
          return route.subscriptionId == command.subscriptionId;
        });
    if (routeIt == runtime.goosePublishRoutes.end() ||
        routeIt->publishers.empty() || command.values.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "IEC61850 GOOSE发布端或数据不能为空");
    }
    auto& route = *routeIt;
    std::uint32_t nextState = route.stateNumber;
    std::uint32_t nextSequence = route.sequenceNumber;
    if (command.stateChanged || !route.hasValues) {
      if (nextState == std::numeric_limits<std::uint32_t>::max()) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "GOOSE发布状态序号即将回绕");
      }
      ++nextState;
      nextSequence = 0;
    } else {
      if (nextSequence == std::numeric_limits<std::uint32_t>::max()) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "GOOSE发布重发序号即将回绕");
      }
      ++nextSequence;
    }
    for (const auto& publisher : route.publishers) {
      const auto status = publisher->PublishWithSequence(
          command.values, nextState, nextSequence);
      if (!status.ok()) {
        return status;
      }
    }
    route.stateNumber = nextState;
    route.sequenceNumber = nextSequence;
    route.hasValues = true;
    route.retransmitActive = true;
    route.retransmitIntervalMs = kGooseInitialRetransmitIntervalMs;
    route.nextRetransmitAt =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(route.retransmitIntervalMs);
    ++runtime.publishRevision;
    runtime.publishCondition.notify_all();
    return grpc::Status::OK;
  }

  grpc::Status ReadMms(std::string_view connName,
                       const MmsReadRequest& request,
                       MmsReadResponse* response) override {
    std::shared_ptr<Runtime> runtime;
    {
      std::lock_guard lock(mutex_);
      const auto found = runtimes_.find(std::string(connName));
      if (found == runtimes_.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "IEC61850 MMS控制连接不存在");
      }
      runtime = found->second;
    }
    if (runtime->mmsWorker == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS通信功能未启用");
    }
    return runtime->mmsWorker->ReadMms(request, response);
  }

  grpc::Status WriteMms(std::string_view connName,
                        const MmsWriteRequest& request,
                        MmsWriteResponse* response) override {
    std::shared_ptr<Runtime> runtime;
    {
      std::lock_guard lock(mutex_);
      const auto found = runtimes_.find(std::string(connName));
      if (found == runtimes_.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "IEC61850 MMS控制连接不存在");
      }
      runtime = found->second;
    }
    if (runtime->mmsWorker == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS通信功能未启用");
    }
    return runtime->mmsWorker->WriteMms(request, response);
  }

  grpc::Status SelectMmsControl(std::string_view connName,
                                const MmsObjectName& controlObject,
                                MmsReadResponse* response) override {
    std::shared_ptr<Runtime> runtime;
    {
      std::lock_guard lock(mutex_);
      const auto found = runtimes_.find(std::string(connName));
      if (found == runtimes_.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "IEC61850 MMS控制连接不存在");
      }
      runtime = found->second;
    }
    if (runtime->mmsWorker == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS通信功能未启用");
    }
    return runtime->mmsWorker->SelectMmsControl(controlObject, response);
  }

  grpc::Status WriteMmsControl(std::string_view connName,
                               const MmsControlCommand& command,
                               MmsWriteResponse* response) override {
    std::shared_ptr<Runtime> runtime;
    {
      std::lock_guard lock(mutex_);
      const auto found = runtimes_.find(std::string(connName));
      if (found == runtimes_.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "IEC61850 MMS控制连接不存在");
      }
      runtime = found->second;
    }
    if (runtime->mmsWorker == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS通信功能未启用");
    }
    return runtime->mmsWorker->WriteMmsControl(command, response);
  }

  grpc::Status ReadSettingGroupStatus(
      std::string_view connName, const MmsSettingGroupPlan& plan,
      MmsSettingGroupStatus* status) override {
    std::shared_ptr<Runtime> runtime;
    {
      std::lock_guard lock(mutex_);
      const auto found = runtimes_.find(std::string(connName));
      if (found == runtimes_.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "IEC61850 SGCB连接不存在");
      }
      runtime = found->second;
    }
    if (runtime->mmsWorker == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 SGCB通信功能未启用");
    }
    return runtime->mmsWorker->ReadSettingGroupStatus(plan, status);
  }

  grpc::Status SelectSettingGroup(std::string_view connName,
                                  const MmsSettingGroupPlan& plan,
                                  std::uint32_t group) override {
    std::shared_ptr<Runtime> runtime;
    {
      std::lock_guard lock(mutex_);
      const auto found = runtimes_.find(std::string(connName));
      if (found == runtimes_.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "IEC61850 SGCB连接不存在");
      }
      runtime = found->second;
    }
    if (runtime->mmsWorker == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 SGCB通信功能未启用");
    }
    return runtime->mmsWorker->SelectSettingGroup(plan, group);
  }

  grpc::Status ConfirmSettingGroupEdit(std::string_view connName,
                                       const MmsSettingGroupPlan& plan) override {
    std::shared_ptr<Runtime> runtime;
    {
      std::lock_guard lock(mutex_);
      const auto found = runtimes_.find(std::string(connName));
      if (found == runtimes_.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "IEC61850 SGCB连接不存在");
      }
      runtime = found->second;
    }
    if (runtime->mmsWorker == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 SGCB通信功能未启用");
    }
    return runtime->mmsWorker->ConfirmSettingGroupEdit(plan);
  }

  grpc::Status CancelSettingGroupEdit(std::string_view connName,
                                      const MmsSettingGroupPlan& plan) override {
    std::shared_ptr<Runtime> runtime;
    {
      std::lock_guard lock(mutex_);
      const auto found = runtimes_.find(std::string(connName));
      if (found == runtimes_.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "IEC61850 SGCB连接不存在");
      }
      runtime = found->second;
    }
    if (runtime->mmsWorker == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 SGCB通信功能未启用");
    }
    return runtime->mmsWorker->CancelSettingGroupEdit(plan);
  }

  grpc::Status ActivateSettingGroup(std::string_view connName,
                                    const MmsSettingGroupPlan& plan,
                                    std::uint32_t group) override {
    std::shared_ptr<Runtime> runtime;
    {
      std::lock_guard lock(mutex_);
      const auto found = runtimes_.find(std::string(connName));
      if (found == runtimes_.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "IEC61850 SGCB连接不存在");
      }
      runtime = found->second;
    }
    if (runtime->mmsWorker == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 SGCB通信功能未启用");
    }
    return runtime->mmsWorker->ActivateSettingGroup(plan, group);
  }

  grpc::Status ExecuteMmsPointControl(
      std::string_view connName, const MmsPointControlCommand& command,
      MmsWriteResponse* response) override {
    std::shared_ptr<Runtime> runtime;
    {
      std::lock_guard lock(mutex_);
      const auto found = runtimes_.find(std::string(connName));
      if (found == runtimes_.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "IEC61850 MMS控制连接不存在");
      }
      runtime = found->second;
    }
    if (runtime->mmsWorker == nullptr) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS通信功能未启用");
    }
    return runtime->mmsWorker->ExecuteMmsPointControl(command, response);
  }

  grpc::Status StopIed(std::string_view connName) override {
    std::vector<std::shared_ptr<Runtime>> runtimes;
    {
      std::lock_guard lock(mutex_);
      if (connName.empty()) {
        for (auto& [name, runtime] : runtimes_) {
          runtimes.emplace_back(std::move(runtime));
        }
        runtimes_.clear();
      } else {
        const auto found = runtimes_.find(std::string(connName));
        if (found != runtimes_.end()) {
          runtimes.emplace_back(std::move(found->second));
          runtimes_.erase(found);
        }
      }
    }
    if (runtimes.empty()) {
      return grpc::Status::OK;
    }
    for (const auto& runtime : runtimes) {
      runtime->Stop();
    }
    LOG_INFO("IEC61850自研IEC61850协议会话已停止");
    return grpc::Status::OK;
  }

private:
  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<Runtime>> runtimes_;
};

std::shared_ptr<ProtocolStackAdapter> MakeRawProtocolStack() {
  return std::make_shared<RawProtocolStackAdapter>();
}

}  // namespace IEC61850
