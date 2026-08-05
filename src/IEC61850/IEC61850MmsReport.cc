#include "IEC61850MmsReport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "IEC61850MmsBer.h"
#include "mskdsp/IEC61850Limits.hpp"

namespace IEC61850 {
namespace {

constexpr std::size_t kMaxMmsPduBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxReportMembers = 65536;
constexpr std::size_t kMaxReportAccessResults = 2 * kMaxReportMembers + 16;
constexpr std::size_t kMaxIdentifierBytes = 1024;
constexpr std::size_t kMaxStructuredDepth = 16;
constexpr std::int64_t kMillisecondsPerDay = 86400000;
// IEC 61850 BinaryTime(6)的日期基准为1984-01-01，距Unix epoch共5113天。
constexpr std::int64_t kDaysFrom1984To1970 = 5113;

grpc::Status Invalid(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::DATA_LOSS,
                      std::format("IEC61850 MMS InformationReport报文无效: {}",
                                  reason));
}

grpc::Status ArgumentError(std::string_view reason) {
  return grpc::Status(
      grpc::StatusCode::INVALID_ARGUMENT,
      std::format("IEC61850 MMS InformationReport参数无效: {}", reason));
}

grpc::Status ResourceError(std::string_view reason) {
  return grpc::Status(
      grpc::StatusCode::RESOURCE_EXHAUSTED,
      std::format("IEC61850 MMS InformationReport超过下位机上限: {}", reason));
}

bool IsContextPrimitive(std::uint8_t tag, std::uint8_t number) noexcept {
  return tag == static_cast<std::uint8_t>(0x80 | number);
}

bool IsContextConstructed(std::uint8_t tag, std::uint8_t number) noexcept {
  return tag == static_cast<std::uint8_t>(0xa0 | number);
}

bool IsMmsDataTag(std::uint8_t tag) noexcept {
  switch (tag) {
    case 0xa1:  // array
    case 0xa2:  // structure
    case 0x83:  // boolean
    case 0x84:  // bit-string
    case 0x85:  // integer
    case 0x86:  // unsigned
    case 0x87:  // floating-point
    case 0x89:  // octet-string
    case 0x8a:  // visible-string
    case 0x8b:  // generalized-time
    case 0x8c:  // binary-time
    case 0x8d:  // bcd
    case 0x8e:  // object-identifier
    case 0x8f:  // MMS string
    case 0x90:  // UTF-8 string
    case 0x91:  // UTC time
      return true;
    default:
      return false;
  }
}

bool IsConstructedDataTag(std::uint8_t tag) noexcept {
  return tag == 0xa1 || tag == 0xa2;
}

struct BitStringView {
  std::uint8_t unusedBits = 0;
  std::span<const std::uint8_t> payload;

  std::size_t bitCount() const noexcept {
    return payload.size() * 8u - unusedBits;
  }
};

grpc::Status ReadBitString(std::span<const std::uint8_t> value,
                           BitStringView* result) {
  if (result == nullptr || value.empty() || value.front() > 7) {
    return Invalid("BIT STRING长度或未使用位数错误");
  }
  const auto unusedBits = value.front();
  const auto payload = value.subspan(1);
  if (payload.empty() && unusedBits != 0) {
    return Invalid("BIT STRING空载荷却声明未使用位");
  }
  if (unusedBits != 0 && !payload.empty() &&
      (payload.back() & static_cast<std::uint8_t>((1u << unusedBits) - 1u)) !=
          0) {
    return Invalid("BIT STRING未使用低位不为零");
  }
  result->unusedBits = unusedBits;
  result->payload = payload;
  return grpc::Status::OK;
}

bool BitAt(const BitStringView& value, std::size_t index) noexcept {
  if (index >= value.bitCount()) {
    return false;
  }
  const auto byteIndex = index / 8;
  const auto bitIndex = index % 8;
  return (value.payload[byteIndex] &
          static_cast<std::uint8_t>(0x80u >> bitIndex)) != 0;
}

std::string NormalizeReference(std::string_view value) {
  std::string result(value);
  for (auto& character : result) {
    if (character == '$') {
      character = '.';
    }
  }
  return result;
}

bool ReferenceMatches(std::string_view received,
                      std::string_view expected) {
  return received == expected ||
         NormalizeReference(received) == NormalizeReference(expected);
}

grpc::Status ReadVisibleString(const BerTlvView& tlv, std::string* result) {
  if (result == nullptr || tlv.tag != 0x8a || tlv.value.empty() ||
      tlv.value.size() > kMaxIdentifierBytes) {
    return Invalid("VisibleString字段错误");
  }
  result->assign(reinterpret_cast<const char*>(tlv.value.data()),
                 tlv.value.size());
  return grpc::Status::OK;
}

grpc::Status ReadUnsigned64(const BerTlvView& tlv, std::uint64_t* result) {
  if (result == nullptr || tlv.tag != 0x86) {
    return Invalid("Unsigned字段标签错误");
  }
  std::uint64_t value = 0;
  const auto status = ReadBerUnsigned(tlv.value, &value);
  if (!status.ok()) {
    return Invalid("Unsigned字段值错误");
  }
  *result = value;
  return grpc::Status::OK;
}

grpc::Status ReadBoolean(const BerTlvView& tlv, bool* result) {
  if (result == nullptr || tlv.tag != 0x83) {
    return Invalid("BOOLEAN字段标签错误");
  }
  const auto status = ReadBerBoolean(tlv.value, result);
  return status.ok() ? grpc::Status::OK : Invalid("BOOLEAN字段值错误");
}

grpc::Status DecodeOptionalFields(const BerTlvView& tlv,
                                  IEC61850Proto::SclOptionalFields* result) {
  if (result == nullptr || tlv.tag != 0x84) {
    return Invalid("OptFlds字段标签错误");
  }
  BitStringView bits;
  auto status = ReadBitString(tlv.value, &bits);
  if (!status.ok() || bits.bitCount() != 10) {
    return Invalid("OptFlds必须是恰好10位BIT STRING");
  }
  if (BitAt(bits, 0)) {
    return Invalid("OptFlds保留位被置位");
  }
  result->Clear();
  result->set_sequence_number(BitAt(bits, 1));
  result->set_report_timestamp(BitAt(bits, 2));
  result->set_reason_code(BitAt(bits, 3));
  result->set_data_set(BitAt(bits, 4));
  result->set_data_reference(BitAt(bits, 5));
  result->set_buffer_overflow(BitAt(bits, 6));
  result->set_entry_id(BitAt(bits, 7));
  result->set_config_revision(BitAt(bits, 8));
  result->set_segmentation(BitAt(bits, 9));
  return grpc::Status::OK;
}

bool OptionalFieldsEqual(const IEC61850Proto::SclOptionalFields& left,
                         const IEC61850Proto::SclOptionalFields& right) {
  return left.SerializeAsString() == right.SerializeAsString();
}

grpc::Status DecodeBinaryTime(std::span<const std::uint8_t> value,
                              std::int64_t* timestampMs) {
  if (timestampMs == nullptr || value.size() != 6) {
    return Invalid("BinaryTime必须是6字节");
  }
  const std::uint32_t days =
      (static_cast<std::uint32_t>(value[0]) << 8) | value[1];
  const std::uint32_t milliseconds =
      (static_cast<std::uint32_t>(value[2]) << 24) |
      (static_cast<std::uint32_t>(value[3]) << 16) |
      (static_cast<std::uint32_t>(value[4]) << 8) | value[5];
  if (milliseconds >= static_cast<std::uint32_t>(kMillisecondsPerDay)) {
    return Invalid("BinaryTime毫秒值超出一天范围");
  }
  if (days > static_cast<std::uint64_t>(
                 (std::numeric_limits<std::int64_t>::max() /
                  kMillisecondsPerDay) - kDaysFrom1984To1970)) {
    return Invalid("BinaryTime日期值溢出");
  }
  *timestampMs =
      (static_cast<std::int64_t>(days) + kDaysFrom1984To1970) *
          kMillisecondsPerDay +
      milliseconds;
  return grpc::Status::OK;
}

grpc::Status DecodeUtcTime(std::span<const std::uint8_t> value,
                           std::int64_t* timestampMs, bool* valid) {
  if (timestampMs == nullptr || valid == nullptr || value.size() != 8) {
    return Invalid("UTC time必须是8字节");
  }
  std::uint64_t seconds = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    seconds = (seconds << 8) | value[index];
  }
  std::uint32_t fraction = 0;
  for (std::size_t index = 4; index < 7; ++index) {
    fraction = (fraction << 8) | value[index];
  }
  if (seconds >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() /
                                 1000)) {
    return Invalid("UTC time秒值溢出");
  }
  const auto fractionMs =
      static_cast<std::uint64_t>(fraction) * 1000u / 0x1000000u;
  *timestampMs = static_cast<std::int64_t>(seconds * 1000u + fractionMs);
  // UTC time quality的bit7为时钟未同步标志。
  *valid = (value[7] & 0x80u) == 0;
  return grpc::Status::OK;
}

grpc::Status DecodeQuality(const BitStringView& bits, MmsQuality* quality) {
  if (quality == nullptr || bits.bitCount() != 13) {
    return Invalid("IEC Quality必须是13位BIT STRING");
  }
  const auto validity = static_cast<unsigned>((BitAt(bits, 0) ? 2 : 0) |
                                               (BitAt(bits, 1) ? 1 : 0));
  switch (validity) {
    case 0:
      quality->validity = MmsValidity::GOOD;
      break;
    case 1:
      quality->validity = MmsValidity::QUESTIONABLE;
      break;
    case 2:
      quality->validity = MmsValidity::INVALID;
      break;
    case 3:
      quality->validity = MmsValidity::RESERVED;
      break;
    default:
      return Invalid("IEC Quality有效性编码错误");
  }
  quality->overflow = BitAt(bits, 2);
  quality->outOfRange = BitAt(bits, 3);
  quality->badReference = BitAt(bits, 4);
  quality->oscillatory = BitAt(bits, 5);
  quality->failure = BitAt(bits, 6);
  quality->oldData = BitAt(bits, 7);
  quality->inconsistent = BitAt(bits, 8);
  quality->inaccurate = BitAt(bits, 9);
  quality->sourceSubstituted = BitAt(bits, 10);
  quality->test = BitAt(bits, 11);
  quality->operatorBlocked = BitAt(bits, 12);
  return grpc::Status::OK;
}

grpc::Status DecodeFloatingPoint(std::span<const std::uint8_t> value,
                                 double* result) {
  if (result == nullptr || value.empty()) {
    return Invalid("FLOATING-POINT值为空");
  }
  if (value.size() == 5 && value.front() == 0x08) {
    std::uint32_t bits = 0;
    for (std::size_t index = 1; index < value.size(); ++index) {
      bits = (bits << 8) | value[index];
    }
    *result = static_cast<double>(std::bit_cast<float>(bits));
    return grpc::Status::OK;
  }
  if (value.size() == 9 && value.front() == 0x0b) {
    std::uint64_t bits = 0;
    for (std::size_t index = 1; index < value.size(); ++index) {
      bits = (bits << 8) | value[index];
    }
    *result = std::bit_cast<double>(bits);
    return grpc::Status::OK;
  }
  return Invalid("FLOATING-POINT格式宽度与值长度不匹配");
}

grpc::Status DecodeDataValue(const BerTlvView& tlv,
                             const MmsReportMember& member,
                             MmsDataValue* result,
                             std::size_t depth = 0) {
  if (result == nullptr || !IsMmsDataTag(tlv.tag)) {
    return Invalid("报告Data选择标签错误");
  }
  if (tlv.value.size() > mskdsp::kIec61850MaxMmsVariableValueBytes) {
    return ResourceError("单个Data值过大");
  }
  result->dataRef = member.dataRef;
  result->fc = member.fc;

  if (member.quality && tlv.tag != 0x84) {
    return Invalid("Quality成员不是BIT STRING");
  }

  if (IsConstructedDataTag(tlv.tag)) {
    if (depth >= kMaxStructuredDepth) {
      return ResourceError("Data结构嵌套层级过深");
    }
    auto composite = std::make_shared<MmsCompositeValue>();
    composite->kind = tlv.tag == 0xa1 ? MmsCompositeValue::Kind::ARRAY
                                      : MmsCompositeValue::Kind::STRUCTURE;
    composite->encodedContent.assign(tlv.value.begin(), tlv.value.end());
    std::size_t nestedOffset = 0;
    while (nestedOffset < tlv.value.size()) {
      if (composite->elements.size() >= kMaxReportMembers) {
        return ResourceError("Data结构成员数量过多");
      }
      BerTlvView child;
      auto status = ReadBerTlv(tlv.value, &nestedOffset, &child);
      if (!status.ok() || !IsMmsDataTag(child.tag)) {
        return Invalid("Data结构成员不是有效MMS Data选择");
      }
      MmsDataValue childValue;
      MmsReportMember childMember;
      status = DecodeDataValue(child, childMember, &childValue, depth + 1);
      if (!status.ok()) {
        return status;
      }
      composite->elements.emplace_back(std::move(childValue.value));
    }
    result->value = std::move(composite);
    return grpc::Status::OK;
  }

  switch (tlv.tag) {
    case 0x83: {
      bool value = false;
      auto status = ReadBerBoolean(tlv.value, &value);
      if (!status.ok()) {
        return Invalid("BOOLEAN Data值错误");
      }
      result->value = value;
      break;
    }
    case 0x84: {
      BitStringView bits;
      auto status = ReadBitString(tlv.value, &bits);
      if (!status.ok()) {
        return status;
      }
      result->value = std::vector<std::uint8_t>(bits.payload.begin(),
                                                bits.payload.end());
      if (member.quality) {
        status = DecodeQuality(bits, &result->quality);
        if (!status.ok()) {
          return status;
        }
      }
      break;
    }
    case 0x85: {
      std::int64_t value = 0;
      auto status = ReadBerSigned(tlv.value, &value);
      if (!status.ok()) {
        return Invalid("INTEGER Data值错误");
      }
      result->value = value;
      break;
    }
    case 0x86: {
      std::uint64_t value = 0;
      auto status = ReadBerUnsigned(tlv.value, &value);
      if (!status.ok() || value > std::numeric_limits<std::int64_t>::max()) {
        return Invalid("UNSIGNED Data值超出内部整数范围");
      }
      result->value = static_cast<std::int64_t>(value);
      break;
    }
    case 0x87: {
      double value = 0.0;
      auto status = DecodeFloatingPoint(tlv.value, &value);
      if (!status.ok()) {
        return status;
      }
      result->value = value;
      break;
    }
    case 0x89:
    case 0x8d:
    case 0x8e:
      result->value = std::vector<std::uint8_t>(tlv.value.begin(),
                                                tlv.value.end());
      break;
    case 0x8a:
    case 0x8b:
    case 0x8f:
    case 0x90:
      result->value = std::string(
          reinterpret_cast<const char*>(tlv.value.data()), tlv.value.size());
      break;
    case 0x8c: {
      if (tlv.value.size() == 6) {
        std::int64_t timestamp = 0;
        auto status = DecodeBinaryTime(tlv.value, &timestamp);
        if (!status.ok()) {
          return status;
        }
        result->value = timestamp;
      } else if (tlv.value.size() == 4) {
        result->value = std::vector<std::uint8_t>(tlv.value.begin(),
                                                  tlv.value.end());
      } else {
        return Invalid("BinaryTime Data值长度错误");
      }
      break;
    }
    case 0x91: {
      std::int64_t timestamp = 0;
      bool valid = false;
      auto status = DecodeUtcTime(tlv.value, &timestamp, &valid);
      if (!status.ok()) {
        return status;
      }
      result->value = timestamp;
      result->timestampMs = timestamp;
      result->timestampValid = valid;
      break;
    }
    default:
      return Invalid("未知MMS Data选择");
  }
  return grpc::Status::OK;
}

std::int64_t CurrentTimestampMs() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

grpc::Status DecodeMmsInformationReport(
    std::span<const std::uint8_t> input, const MmsReportDecodePlan& plan,
    MmsReportSegment* result) {
  if (result == nullptr) {
    return ArgumentError("报告输出参数为空");
  }
  *result = {};
  if (input.empty() || input.size() > kMaxMmsPduBytes) {
    return ArgumentError("报告输入为空或超过PDU上限");
  }
  if (plan.reportRef.empty() || plan.reportId.empty() ||
      plan.dataSetRef.empty() || plan.members.size() > kMaxReportMembers) {
    return ArgumentError("报告解码计划身份或成员数量无效");
  }
  std::size_t offset = 0;
  BerTlvView outer;
  auto status = ReadBerTlv(input, &offset, &outer);
  if (!status.ok() || offset != input.size() || outer.tag != 0xa3) {
    return Invalid("缺少Unconfirmed-PDU[3]");
  }
  std::size_t unconfirmedOffset = 0;
  BerTlvView reportChoice;
  status = ReadBerTlv(outer.value, &unconfirmedOffset, &reportChoice);
  if (!status.ok() || unconfirmedOffset != outer.value.size() ||
      reportChoice.tag != 0xa0) {
    return Invalid("缺少InformationReport[0]");
  }

  std::size_t reportOffset = 0;
  BerTlvView variableAccess;
  status = ReadBerTlv(reportChoice.value, &reportOffset, &variableAccess);
  if (!status.ok() || variableAccess.tag != 0xa1) {
    return Invalid("InformationReport缺少variable-list-name[1]");
  }
  std::size_t variableNameOffset = 0;
  BerTlvView variableName;
  status = ReadBerTlv(variableAccess.value, &variableNameOffset, &variableName);
  const std::array<std::uint8_t, 3> kReportVariableListName{'R', 'P', 'T'};
  if (!status.ok() || variableNameOffset != variableAccess.value.size() ||
      !IsContextPrimitive(variableName.tag, 0) ||
      variableName.value.size() != kReportVariableListName.size() ||
      !std::equal(variableName.value.begin(), variableName.value.end(),
                  kReportVariableListName.begin())) {
    return Invalid("InformationReport变量列表名称不是VMD对象RPT");
  }

  BerTlvView accessList;
  status = ReadBerTlv(reportChoice.value, &reportOffset, &accessList);
  if (!status.ok() || reportOffset != reportChoice.value.size() ||
      !IsContextConstructed(accessList.tag, 0)) {
    return Invalid("InformationReport缺少listOfAccessResult[0]");
  }

  std::vector<BerTlvView> accessResults;
  accessResults.reserve(std::min<std::size_t>(kMaxReportAccessResults, 256));
  std::size_t listOffset = 0;
  while (listOffset < accessList.value.size()) {
    if (accessResults.size() >= kMaxReportAccessResults) {
      return ResourceError("AccessResult数量过多");
    }
    BerTlvView accessResult;
    status = ReadBerTlv(accessList.value, &listOffset, &accessResult);
    if (!status.ok()) {
      return status;
    }
    if (accessResult.tag == 0x80) {
      return Invalid("InformationReport包含DataAccessError");
    }
    if (!IsMmsDataTag(accessResult.tag)) {
      return Invalid("InformationReport包含未知AccessResult选择");
    }
    accessResults.push_back(accessResult);
  }

  std::size_t valueIndex = 0;
  auto readAccessResult = [&](BerTlvView* tlv) -> grpc::Status {
    if (tlv == nullptr || valueIndex >= accessResults.size()) {
      return Invalid("InformationReportAccessResult数量不足");
    }
    *tlv = accessResults[valueIndex++];
    return grpc::Status::OK;
  };

  BerTlvView rptIdTlv;
  status = readAccessResult(&rptIdTlv);
  if (!status.ok()) {
    return status;
  }
  std::string receivedReportId;
  status = ReadVisibleString(rptIdTlv, &receivedReportId);
  if (!status.ok() || receivedReportId != plan.reportId) {
    return Invalid("RptID与当前RCB计划不匹配");
  }

  BerTlvView optFldsTlv;
  status = readAccessResult(&optFldsTlv);
  if (!status.ok()) {
    return status;
  }
  IEC61850Proto::SclOptionalFields receivedOptionalFields;
  status = DecodeOptionalFields(optFldsTlv, &receivedOptionalFields);
  if (!status.ok() ||
      !OptionalFieldsEqual(receivedOptionalFields, plan.optionalFields)) {
    return Invalid("OptFlds与当前RCB计划不匹配");
  }

  result->reportRef = plan.reportRef;
  result->dataSetRef = plan.dataSetRef;
  result->confRev = plan.confRev;
  result->receiveTimestampMs = CurrentTimestampMs();

  std::int64_t reportTimestampMs = 0;
  bool reportTimestampValid = false;
  bool bufferOverflow = false;
  if (receivedOptionalFields.sequence_number()) {
    BerTlvView sequence;
    status = readAccessResult(&sequence);
    std::uint64_t receivedSequence = 0;
    if (!status.ok() || !ReadUnsigned64(sequence, &receivedSequence).ok()) {
      return Invalid("SqNum字段错误");
    }
    result->sequenceNumber = receivedSequence;
  }
  if (receivedOptionalFields.report_timestamp()) {
    BerTlvView timestamp;
    status = readAccessResult(&timestamp);
    if (!status.ok() || timestamp.tag != 0x8c) {
      return Invalid("TimeOfEntry字段错误");
    }
    status = DecodeBinaryTime(timestamp.value, &reportTimestampMs);
    if (!status.ok()) {
      return status;
    }
    reportTimestampValid = true;
  }
  if (receivedOptionalFields.data_set()) {
    BerTlvView dataSet;
    status = readAccessResult(&dataSet);
    std::string receivedDataSet;
    if (!status.ok() || !ReadVisibleString(dataSet, &receivedDataSet).ok() ||
        !ReferenceMatches(receivedDataSet, plan.dataSetRef)) {
      return Invalid("DatSet与当前DataSet计划不匹配");
    }
  }
  if (receivedOptionalFields.buffer_overflow()) {
    BerTlvView overflow;
    status = readAccessResult(&overflow);
    if (!status.ok() || !ReadBoolean(overflow, &bufferOverflow).ok()) {
      return Invalid("BufOvfl字段错误");
    }
  }
  if (receivedOptionalFields.entry_id()) {
    BerTlvView entryId;
    status = readAccessResult(&entryId);
    if (!status.ok() || entryId.tag != 0x89 || entryId.value.size() != 8) {
      return Invalid("EntryID字段必须是8字节OCTET STRING");
    }
  }
  if (receivedOptionalFields.config_revision()) {
    BerTlvView confRev;
    status = readAccessResult(&confRev);
    std::uint64_t receivedConfRev = 0;
    if (!status.ok() || !ReadUnsigned64(confRev, &receivedConfRev).ok() ||
        receivedConfRev != plan.confRev) {
      return Invalid("ConfRev与当前RCB计划不匹配");
    }
  }
  if (receivedOptionalFields.segmentation()) {
    BerTlvView subSequence;
    status = readAccessResult(&subSequence);
    std::uint64_t receivedSubSequence = 0;
    if (!status.ok() || !ReadUnsigned64(subSequence, &receivedSubSequence).ok() ||
        receivedSubSequence > std::numeric_limits<std::uint32_t>::max()) {
      return Invalid("SubSeqNum字段错误");
    }
    result->segmentNumber = static_cast<std::uint32_t>(receivedSubSequence);
    BerTlvView moreSegments;
    status = readAccessResult(&moreSegments);
    if (!status.ok() || !ReadBoolean(moreSegments, &result->moreSegmentsFollow)
                          .ok()) {
      return Invalid("MoreSegmentsFollow字段错误");
    }
  }

  BerTlvView inclusionTlv;
  status = readAccessResult(&inclusionTlv);
  if (!status.ok() || inclusionTlv.tag != 0x84) {
    return Invalid("InformationReport缺少Inclusion BIT STRING");
  }
  BitStringView inclusion;
  status = ReadBitString(inclusionTlv.value, &inclusion);
  if (!status.ok() || inclusion.bitCount() != plan.members.size()) {
    return Invalid("Inclusion位数与DataSet成员数量不一致");
  }

  std::vector<std::size_t> includedMembers;
  includedMembers.reserve(plan.members.size());
  for (std::size_t index = 0; index < plan.members.size(); ++index) {
    if (BitAt(inclusion, index)) {
      includedMembers.push_back(index);
    }
  }

  if (receivedOptionalFields.data_reference()) {
    for (const auto memberIndex : includedMembers) {
      BerTlvView dataRef;
      status = readAccessResult(&dataRef);
      std::string receivedDataRef;
      if (!status.ok() || !ReadVisibleString(dataRef, &receivedDataRef).ok() ||
          !ReferenceMatches(receivedDataRef,
                            plan.members[memberIndex].dataRef)) {
        return Invalid("DataRef与DataSet成员顺序或引用不匹配");
      }
    }
  }

  result->values.reserve(includedMembers.size());
  for (const auto memberIndex : includedMembers) {
    BerTlvView data;
    status = readAccessResult(&data);
    if (!status.ok()) {
      return status;
    }
    MmsDataValue decoded;
    status = DecodeDataValue(data, plan.members[memberIndex], &decoded);
    if (!status.ok()) {
      return status;
    }
    decoded.timestampMs = reportTimestampMs;
    decoded.timestampValid = reportTimestampValid;
    if (bufferOverflow) {
      decoded.quality.oldData = true;
    }
    result->values.emplace_back(std::move(decoded));
  }

  if (receivedOptionalFields.reason_code()) {
    for (std::size_t index = 0; index < includedMembers.size(); ++index) {
      BerTlvView reason;
      status = readAccessResult(&reason);
      if (!status.ok() || reason.tag != 0x84) {
        return Invalid("Reason字段必须是BIT STRING");
      }
      BitStringView reasonBits;
      status = ReadBitString(reason.value, &reasonBits);
      if (!status.ok() || reasonBits.bitCount() == 0 ||
          reasonBits.bitCount() > 8) {
        return Invalid("Reason位数超出IEC 61850范围");
      }
      // Reason bit4表示general-interrogation；分段合并器会对各段结果做或运算。
      if (reasonBits.bitCount() > 4 && BitAt(reasonBits, 4)) {
        result->generalInterrogation = true;
      }
    }
  }
  if (valueIndex != accessResults.size()) {
    return Invalid("InformationReport包含未消费的AccessResult");
  }
  return grpc::Status::OK;
}

}  // namespace IEC61850
