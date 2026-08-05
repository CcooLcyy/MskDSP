#include "IEC61850MmsService.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>

#include "IEC61850MmsBer.h"

namespace IEC61850 {
namespace {

constexpr std::size_t kMaxOptionalBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxJournalEntries = 4096;
constexpr std::size_t kMaxJournalVariables = 4096;
constexpr std::size_t kMaxIdentifier = 1024;

grpc::Status Invalid(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::DATA_LOSS,
                      std::string("IEC61850 MMS扩展报文无效: ") +
                          std::string(reason));
}

grpc::Status Argument(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      std::string("IEC61850 MMS扩展参数无效: ") +
                          std::string(reason));
}

grpc::Status Resource(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                      std::string("IEC61850 MMS扩展资源超限: ") +
                          std::string(reason));
}

bool IsConstructed(std::uint32_t tag, std::uint32_t number) {
  return tag == (0xa0U | number);
}

grpc::Status Append(std::uint32_t tag, std::span<const std::uint8_t> value,
                    std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return Argument("BER输出为空");
  }
  if (value.size() > kMaxOptionalBytes) {
    return Resource("BER字段超过MMS上限");
  }
  output->assign(value.size() + 16, 0);
  BerWriter writer(*output);
  if (!writer.Tlv(tag, value)) {
    output->clear();
    return Resource("BER编码失败");
  }
  output->resize(writer.size());
  return grpc::Status::OK;
}

grpc::Status AppendString(std::uint32_t tag, std::string_view value,
                          std::vector<std::uint8_t>* output) {
  if (value.empty() || value.size() > kMaxIdentifier) {
    return Argument("字符串为空或过长");
  }
  return Append(tag, std::span<const std::uint8_t>(
                         reinterpret_cast<const std::uint8_t*>(value.data()),
                         value.size()),
                output);
}

grpc::Status AppendUnsigned(std::uint32_t tag, std::uint64_t value,
                            std::vector<std::uint8_t>* output) {
  output->assign(16, 0);
  BerWriter writer(*output);
  if (!writer.Unsigned(static_cast<std::uint8_t>(tag), value)) {
    output->clear();
    return Resource("无符号整数编码失败");
  }
  output->resize(writer.size());
  return grpc::Status::OK;
}

grpc::Status AppendSigned(std::uint32_t tag, std::int64_t value,
                          std::vector<std::uint8_t>* output) {
  output->assign(16, 0);
  BerWriter writer(*output);
  if (!writer.Signed(static_cast<std::uint8_t>(tag), value)) {
    output->clear();
    return Resource("有符号整数编码失败");
  }
  output->resize(writer.size());
  return grpc::Status::OK;
}

grpc::Status Copy(std::span<const std::uint8_t> input,
                  std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return Argument("输出长度为空");
  }
  *outputSize = 0;
  if (input.size() > output.size()) {
    return Resource("输出缓冲不足");
  }
  std::copy(input.begin(), input.end(), output.begin());
  *outputSize = input.size();
  return grpc::Status::OK;
}

grpc::Status EncodeObject(const MmsObjectName& name,
                          std::vector<std::uint8_t>* output) {
  if (output == nullptr || name.identifier.empty() ||
      name.identifier.size() > kMaxIdentifier) {
    return Argument("对象名无效");
  }
  std::vector<std::uint8_t> id;
  auto status = AppendString(0x1a, name.identifier, &id);
  if (!status.ok()) {
    return status;
  }
  switch (name.type) {
    case MmsObjectNameType::VMD_SPECIFIC:
      return Append(0x80, std::span<const std::uint8_t>(
                                reinterpret_cast<const std::uint8_t*>(
                                    name.identifier.data()),
                                name.identifier.size()),
                    output);
    case MmsObjectNameType::AA_SPECIFIC:
      return Append(0x82, std::span<const std::uint8_t>(
                                reinterpret_cast<const std::uint8_t*>(
                                    name.identifier.data()),
                                name.identifier.size()),
                    output);
    case MmsObjectNameType::DOMAIN_SPECIFIC: {
      if (name.domain.empty() || name.domain.size() > kMaxIdentifier) {
        return Argument("Domain对象名域标识无效");
      }
      std::vector<std::uint8_t> domain;
      status = AppendString(0x1a, name.domain, &domain);
      if (!status.ok()) {
        return status;
      }
      domain.insert(domain.end(), id.begin(), id.end());
      return Append(0xa1, domain, output);
    }
  }
  return Argument("对象名类型未知");
}

grpc::Status DecodeObject(std::span<const std::uint8_t> input,
                          MmsObjectName* output) {
  if (output == nullptr) {
    return Argument("对象名输出为空");
  }
  *output = {};
  std::size_t offset = 0;
  BerTlvView outer;
  auto status = ReadBerTlv(input, &offset, &outer);
  if (!status.ok() || offset != input.size()) {
    return Invalid("对象名TLV边界错误");
  }
  if (outer.tag == 0x80 || outer.tag == 0x82) {
    if (outer.value.empty() || outer.value.size() > kMaxIdentifier) {
      return Invalid("对象名标识无效");
    }
    output->type = outer.tag == 0x80 ? MmsObjectNameType::VMD_SPECIFIC
                                     : MmsObjectNameType::AA_SPECIFIC;
    output->identifier.assign(reinterpret_cast<const char*>(outer.value.data()),
                              outer.value.size());
    return grpc::Status::OK;
  }
  if (outer.tag != 0xa1) {
    return Invalid("对象名选择错误");
  }
  std::size_t innerOffset = 0;
  BerTlvView domain;
  BerTlvView item;
  status = ReadBerTlv(outer.value, &innerOffset, &domain);
  if (!status.ok() || domain.tag != 0x1a || domain.value.empty()) {
    return Invalid("Domain对象名域标识错误");
  }
  status = ReadBerTlv(outer.value, &innerOffset, &item);
  if (!status.ok() || item.tag != 0x1a || item.value.empty() ||
      innerOffset != outer.value.size()) {
    return Invalid("Domain对象名条目标识错误");
  }
  output->type = MmsObjectNameType::DOMAIN_SPECIFIC;
  output->domain.assign(reinterpret_cast<const char*>(domain.value.data()),
                        domain.value.size());
  output->identifier.assign(reinterpret_cast<const char*>(item.value.data()),
                            item.value.size());
  return grpc::Status::OK;
}

grpc::Status EncodeObjectRequest(std::uint32_t invokeId, std::uint32_t tag,
                                 const MmsObjectName& name,
                                 std::span<std::uint8_t> output,
                                 std::size_t* outputSize) {
  std::vector<std::uint8_t> object;
  auto status = EncodeObject(name, &object);
  if (!status.ok()) {
    return status;
  }
  return EncodeMmsConfirmedRequest(invokeId, tag, object, output, outputSize);
}

grpc::Status DecodeObjectRequest(std::span<const std::uint8_t> input,
                                 std::uint32_t expectedTag,
                                 std::uint32_t* invokeId,
                                 MmsObjectName* name) {
  if (invokeId == nullptr || name == nullptr) {
    return Argument("对象名请求输出为空");
  }
  *invokeId = 0;
  *name = {};
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedRequest(input, &pdu);
  if (!status.ok() || pdu.serviceTag != expectedTag) {
    return Invalid("对象名请求服务选择错误");
  }
  MmsObjectName decoded;
  status = DecodeObject(pdu.serviceValue, &decoded);
  if (!status.ok()) {
    return status;
  }
  *invokeId = pdu.invokeId;
  *name = std::move(decoded);
  return grpc::Status::OK;
}

grpc::Status EncodeBinaryTimeValue(std::int64_t timestampMs,
                                   std::vector<std::uint8_t>* value) {
  std::vector<std::uint8_t> encoded;
  auto status = EncodeMmsDataBinaryTime(timestampMs, &encoded);
  if (!status.ok()) {
    return status;
  }
  std::size_t offset = 0;
  BerTlvView tlv;
  status = ReadBerTlv(encoded, &offset, &tlv);
  if (!status.ok() || offset != encoded.size() || tlv.tag != 0x8c) {
    return Invalid("BinaryTime编码失败");
  }
  value->assign(tlv.value.begin(), tlv.value.end());
  return grpc::Status::OK;
}

grpc::Status DecodeBinaryTimeValue(std::span<const std::uint8_t> value,
                                   std::int64_t* timestampMs) {
  if (timestampMs == nullptr || value.size() != 6) {
    return Invalid("BinaryTime长度无效");
  }
  constexpr std::int64_t kDaysFrom1984To1970 = 5113;
  const auto days = (static_cast<std::uint32_t>(value[0]) << 8) | value[1];
  const auto milliseconds = (static_cast<std::uint32_t>(value[2]) << 24) |
                            (static_cast<std::uint32_t>(value[3]) << 16) |
                            (static_cast<std::uint32_t>(value[4]) << 8) |
                            value[5];
  if (milliseconds >= 86400000U) {
    return Invalid("BinaryTime日内毫秒超出范围");
  }
  const auto elapsedDays = static_cast<std::int64_t>(days);
  if (elapsedDays >
      (std::numeric_limits<std::int64_t>::max() / 1000 -
       kDaysFrom1984To1970 * 86400 - milliseconds / 1000)) {
    return Invalid("BinaryTime超出毫秒范围");
  }
  *timestampMs = (elapsedDays + kDaysFrom1984To1970) * 86400000LL +
                 milliseconds;
  return grpc::Status::OK;
}

grpc::Status EncodeIntegerValue(std::int64_t value,
                                std::vector<std::uint8_t>* output) {
  output->assign(16, 0);
  BerWriter writer(*output);
  if (!writer.Signed(0x02, value)) {
    output->clear();
    return Resource("整数编码失败");
  }
  output->resize(writer.size());
  std::size_t offset = 0;
  BerTlvView tlv;
  auto status = ReadBerTlv(*output, &offset, &tlv);
  if (!status.ok() || offset != output->size()) {
    return Invalid("整数编码失败");
  }
  output->assign(tlv.value.begin(), tlv.value.end());
  return grpc::Status::OK;
}

grpc::Status DecodePrimitive(std::span<const std::uint8_t> input,
                             std::uint32_t tag, std::vector<std::uint8_t>* out,
                             std::size_t* offset) {
  BerTlvView field;
  auto status = ReadBerTlv(input, offset, &field);
  if (!status.ok() || field.tag != tag) {
    return Invalid("字段标签或顺序错误");
  }
  if (field.value.size() > kMaxOptionalBytes) {
    return Resource("字段长度超过上限");
  }
  out->assign(field.value.begin(), field.value.end());
  return grpc::Status::OK;
}

grpc::Status DecodeOptionalTime(std::span<const std::uint8_t> input,
                                std::size_t* offset,
                                std::optional<std::int64_t>* output) {
  if (*offset >= input.size()) {
    return grpc::Status::OK;
  }
  BerTlvView field;
  auto status = ReadBerTlv(input, offset, &field);
  if (!status.ok() || field.tag != 0x80) {
    return Invalid("Journal时间字段错误");
  }
  std::int64_t decoded = 0;
  status = DecodeBinaryTimeValue(field.value, &decoded);
  if (!status.ok()) {
    return status;
  }
  *output = decoded;
  return grpc::Status::OK;
}

grpc::Status DecodeJournalEntry(std::span<const std::uint8_t> input,
                                MmsJournalEntry* output) {
  if (output == nullptr) {
    return Argument("Journal条目输出为空");
  }
  *output = {};
  std::size_t offset = 0;
  BerTlvView entry;
  auto status = ReadBerTlv(input, &offset, &entry);
  if (!status.ok() || offset != input.size() || entry.tag != 0x30) {
    return Invalid("Journal条目结构错误");
  }
  std::size_t inner = 0;
  BerTlvView field;
  status = ReadBerTlv(entry.value, &inner, &field);
  if (!status.ok() || field.tag != 0x80 || field.value.empty() ||
      field.value.size() > 8) {
    return Invalid("Journal EntryID无效");
  }
  output->entryId.assign(field.value.begin(), field.value.end());
  status = ReadBerTlv(entry.value, &inner, &field);
  if (!status.ok() || field.tag != 0x81) {
    return Invalid("Journal发生时间字段错误");
  }
  status = DecodeBinaryTimeValue(field.value, &output->occurrenceTimeMs);
  if (!status.ok()) {
    return status;
  }
  status = ReadBerTlv(entry.value, &inner, &field);
  if (!status.ok() || field.tag != 0x82) {
    return Invalid("Journal条目类型字段错误");
  }
  std::uint64_t kind = 0;
  status = ReadBerUnsigned(field.value, &kind);
  if (!status.ok() || kind > 4) {
    return Invalid("Journal条目类型无效");
  }
  output->kind = static_cast<MmsJournalEntryKind>(kind);
  while (inner < entry.value.size()) {
    status = ReadBerTlv(entry.value, &inner, &field);
    if (!status.ok()) {
      return status;
    }
    if (field.tag == 0x83 || field.tag == 0x85) {
      if (field.value.empty() || field.value.size() > kMaxIdentifier) {
        return Invalid("Journal文本字段无效");
      }
      auto* target = field.tag == 0x83 ? &output->eventCondition
                                      : &output->annotation;
      target->assign(reinterpret_cast<const char*>(field.value.data()),
                     field.value.size());
      continue;
    }
    if (field.tag == 0x84) {
      std::int64_t state = 0;
      status = ReadBerSigned(field.value, &state);
      if (!status.ok()) {
        return status;
      }
      if (state < std::numeric_limits<std::int32_t>::min() ||
          state > std::numeric_limits<std::int32_t>::max()) {
        return Invalid("Journal当前状态超出范围");
      }
      output->currentState = static_cast<std::int32_t>(state);
      continue;
    }
    if (field.tag == 0x87) {
      output->originatingAe.assign(field.value.begin(), field.value.end());
      continue;
    }
    if (field.tag != 0xa6) {
      return Invalid("Journal条目包含未知字段");
    }
    std::size_t variableOffset = 0;
    while (variableOffset < field.value.size()) {
      if (output->variables.size() >= kMaxJournalVariables) {
        return Resource("Journal变量数量超过上限");
      }
      BerTlvView variable;
      status = ReadBerTlv(field.value, &variableOffset, &variable);
      if (!status.ok() || variable.tag != 0x30) {
        return Invalid("Journal变量结构错误");
      }
      std::size_t itemOffset = 0;
      BerTlvView item;
      status = ReadBerTlv(variable.value, &itemOffset, &item);
      if (!status.ok() || item.tag != 0x80 || item.value.empty()) {
        return Invalid("Journal变量标签缺失");
      }
      MmsJournalVariable decoded;
      decoded.tag.assign(reinterpret_cast<const char*>(item.value.data()),
                         item.value.size());
      status = ReadBerTlv(variable.value, &itemOffset, &item);
      if (!status.ok() || item.tag != 0x81 || item.value.empty()) {
        return Invalid("Journal变量Data缺失");
      }
      std::size_t dataOffset = 0;
      BerTlvView data;
      status = ReadBerTlv(item.value, &dataOffset, &data);
      if (!status.ok() || dataOffset != item.value.size()) {
        return Invalid("Journal变量Data结构错误");
      }
      decoded.encodedData.assign(item.value.begin(), item.value.end());
      if (itemOffset < variable.value.size()) {
        status = ReadBerTlv(variable.value, &itemOffset, &item);
        if (!status.ok() || item.tag != 0x82) {
          return Invalid("Journal变量ReasonCode字段错误");
        }
        std::uint64_t reason = 0;
        status = ReadBerUnsigned(item.value, &reason);
        if (!status.ok() || reason > std::numeric_limits<std::uint32_t>::max()) {
          return Invalid("Journal变量ReasonCode无效");
        }
        decoded.reasonCode = static_cast<std::uint32_t>(reason);
      }
      if (itemOffset != variable.value.size()) {
        return Invalid("Journal变量包含多余字段");
      }
      output->variables.emplace_back(std::move(decoded));
    }
  }
  return grpc::Status::OK;
}

grpc::Status EncodeJournalEntry(const MmsJournalEntry& entry,
                                std::vector<std::uint8_t>* output) {
  if (output == nullptr || entry.entryId.empty() || entry.entryId.size() > 8 ||
      entry.kind == MmsJournalEntryKind::UNKNOWN) {
    return Argument("Journal条目参数无效");
  }
  std::vector<std::uint8_t> content;
  std::vector<std::uint8_t> field;
  auto status = Append(0x80, entry.entryId, &field);
  if (!status.ok()) return status;
  content.insert(content.end(), field.begin(), field.end());
  std::vector<std::uint8_t> time;
  status = EncodeBinaryTimeValue(entry.occurrenceTimeMs, &time);
  if (!status.ok()) return status;
  status = Append(0x81, time, &field);
  if (!status.ok()) return status;
  content.insert(content.end(), field.begin(), field.end());
  status = AppendUnsigned(0x82, static_cast<std::uint64_t>(entry.kind), &field);
  if (!status.ok()) return status;
  content.insert(content.end(), field.begin(), field.end());
  if (!entry.eventCondition.empty()) {
    status = AppendString(0x83, entry.eventCondition, &field);
    if (!status.ok()) return status;
    content.insert(content.end(), field.begin(), field.end());
  }
  if (entry.currentState != 0) {
    status = AppendSigned(0x84, entry.currentState, &field);
    if (!status.ok()) return status;
    content.insert(content.end(), field.begin(), field.end());
  }
  if (!entry.annotation.empty()) {
    status = AppendString(0x85, entry.annotation, &field);
    if (!status.ok()) return status;
    content.insert(content.end(), field.begin(), field.end());
  }
  if (!entry.originatingAe.empty()) {
    status = Append(0x87, entry.originatingAe, &field);
    if (!status.ok()) return status;
    content.insert(content.end(), field.begin(), field.end());
  }
  if (!entry.variables.empty()) {
    std::vector<std::uint8_t> variables;
    for (const auto& variable : entry.variables) {
      if (variable.tag.empty() || variable.encodedData.empty()) {
        return Argument("Journal变量参数无效");
      }
      std::vector<std::uint8_t> item;
      status = AppendString(0x80, variable.tag, &field);
      if (!status.ok()) return status;
      item.insert(item.end(), field.begin(), field.end());
      status = Append(0x81, variable.encodedData, &field);
      if (!status.ok()) return status;
      item.insert(item.end(), field.begin(), field.end());
      status = AppendUnsigned(0x82, variable.reasonCode, &field);
      if (!status.ok()) return status;
      item.insert(item.end(), field.begin(), field.end());
      status = Append(0x30, item, &field);
      if (!status.ok()) return status;
      variables.insert(variables.end(), field.begin(), field.end());
    }
    status = Append(0xa6, variables, &field);
    if (!status.ok()) return status;
    content.insert(content.end(), field.begin(), field.end());
  }
  return Append(0x30, content, output);
}

}  // namespace

grpc::Status EncodeMmsJournalStatusRequest(
    std::uint32_t invokeId, const MmsJournalStatusRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeObjectRequest(invokeId, 68, request.journal, output, outputSize);
}

grpc::Status DecodeMmsJournalStatusRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsJournalStatusRequest* request) {
  if (request == nullptr) return Argument("JournalStatus请求输出为空");
  return DecodeObjectRequest(input, 68, invokeId, &request->journal);
}

grpc::Status EncodeMmsJournalStatusResponse(
    std::uint32_t invokeId, const MmsJournalStatusResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  std::vector<std::uint8_t> content;
  std::vector<std::uint8_t> field;
  auto status = AppendUnsigned(0x80, response.currentEntries, &field);
  if (!status.ok()) return status;
  content.insert(content.end(), field.begin(), field.end());
  status = Append(0x81, std::array<std::uint8_t, 1>{
                             static_cast<std::uint8_t>(response.mmsDeletable ? 0xff : 0)},
                  &field);
  if (!status.ok()) return status;
  content.insert(content.end(), field.begin(), field.end());
  return EncodeMmsConfirmedResponse(invokeId, 68, content, output, outputSize);
}

grpc::Status DecodeMmsJournalStatusResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsJournalStatusResponse* response) {
  if (response == nullptr) return Argument("JournalStatus响应输出为空");
  *response = {};
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedResponse(input, &pdu);
  if (!status.ok() || pdu.invokeId != expectedInvokeId || pdu.serviceTag != 68) {
    return Invalid("JournalStatus响应invokeID或服务选择错误");
  }
  std::size_t offset = 0;
  BerTlvView entries;
  status = ReadBerTlv(pdu.serviceValue, &offset, &entries);
  if (!status.ok() || entries.tag != 0x80) return Invalid("JournalStatus条目数错误");
  std::uint64_t count = 0;
  status = ReadBerUnsigned(entries.value, &count);
  if (!status.ok() || count > std::numeric_limits<std::uint32_t>::max()) return Invalid("JournalStatus条目数超限");
  BerTlvView deletable;
  status = ReadBerTlv(pdu.serviceValue, &offset, &deletable);
  bool value = false;
  if (!status.ok() || deletable.tag != 0x81 || !ReadBerBoolean(deletable.value, &value).ok() || offset != pdu.serviceValue.size()) return Invalid("JournalStatus可删除标志错误");
  response->currentEntries = static_cast<std::uint32_t>(count);
  response->mmsDeletable = value;
  return grpc::Status::OK;
}

grpc::Status EncodeMmsInitializeJournalRequest(
    std::uint32_t invokeId, const MmsInitializeJournalRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  std::vector<std::uint8_t> object;
  auto status = EncodeObject(request.journal, &object);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> content = object;
  std::vector<std::uint8_t> field;
  if (request.limitTimeMs.has_value()) {
    std::vector<std::uint8_t> time;
    status = EncodeBinaryTimeValue(*request.limitTimeMs, &time);
    if (!status.ok()) return status;
    status = Append(0x80, time, &field);
    if (!status.ok()) return status;
    content.insert(content.end(), field.begin(), field.end());
  }
  if (!request.limitEntryId.empty()) {
    if (request.limitEntryId.size() > 8) return Argument("Journal EntryID过长");
    status = Append(0x81, request.limitEntryId, &field);
    if (!status.ok()) return status;
    content.insert(content.end(), field.begin(), field.end());
  }
  return EncodeMmsConfirmedRequest(invokeId, 67, content, output, outputSize);
}

grpc::Status DecodeMmsInitializeJournalRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsInitializeJournalRequest* request) {
  if (request == nullptr || invokeId == nullptr) return Argument("InitializeJournal请求输出为空");
  *request = {};
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedRequest(input, &pdu);
  if (!status.ok() || pdu.serviceTag != 67) return Invalid("InitializeJournal服务选择错误");
  std::size_t offset = 0;
  BerTlvView object;
  status = ReadBerTlv(pdu.serviceValue, &offset, &object);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> objectEncoded;
  status = Append(object.tag, object.value, &objectEncoded);
  if (!status.ok()) return status;
  MmsObjectName name;
  status = DecodeObject(objectEncoded, &name);
  if (!status.ok()) return status;
  request->journal = std::move(name);
  while (offset < pdu.serviceValue.size()) {
    BerTlvView field;
    status = ReadBerTlv(pdu.serviceValue, &offset, &field);
    if (!status.ok()) return status;
    if (field.tag == 0x80) {
      std::int64_t time = 0;
      status = DecodeBinaryTimeValue(field.value, &time);
      if (!status.ok()) return status;
      request->limitTimeMs = time;
    } else if (field.tag == 0x81) {
      if (field.value.empty() || field.value.size() > 8) return Invalid("InitializeJournal EntryID无效");
      request->limitEntryId.assign(field.value.begin(), field.value.end());
    } else {
      return Invalid("InitializeJournal包含未知字段");
    }
  }
  *invokeId = pdu.invokeId;
  return grpc::Status::OK;
}

grpc::Status EncodeMmsInitializeJournalResponse(
    std::uint32_t invokeId, const MmsInitializeJournalResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  std::vector<std::uint8_t> value;
  auto status = AppendUnsigned(0x80, response.deletedEntries, &value);
  if (!status.ok()) return status;
  return EncodeMmsConfirmedResponse(invokeId, 67, value, output, outputSize);
}

grpc::Status DecodeMmsInitializeJournalResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsInitializeJournalResponse* response) {
  if (response == nullptr) return Argument("InitializeJournal响应输出为空");
  *response = {};
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedResponse(input, &pdu);
  if (!status.ok() || pdu.invokeId != expectedInvokeId || pdu.serviceTag != 67) return Invalid("InitializeJournal响应服务选择错误");
  std::size_t offset = 0; BerTlvView field; status = ReadBerTlv(pdu.serviceValue, &offset, &field);
  std::uint64_t count = 0;
  if (!status.ok() || field.tag != 0x80 || !ReadBerUnsigned(field.value, &count).ok() || offset != pdu.serviceValue.size() || count > std::numeric_limits<std::uint32_t>::max()) return Invalid("InitializeJournal删除计数错误");
  response->deletedEntries = static_cast<std::uint32_t>(count);
  return grpc::Status::OK;
}

grpc::Status EncodeMmsJournalReadRequest(
    std::uint32_t invokeId, const MmsJournalReadRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  std::vector<std::uint8_t> content;
  auto status = EncodeObject(request.journal, &content);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> field;
  if (request.startTimeMs.has_value()) {
    std::vector<std::uint8_t> time; status = EncodeBinaryTimeValue(*request.startTimeMs, &time); if (!status.ok()) return status;
    status = Append(0x80, time, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end());
  } else if (!request.startEntryId.empty()) {
    if (request.startEntryId.size() > 8) return Argument("Journal起始EntryID过长");
    status = Append(0x81, request.startEntryId, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end());
  }
  if (request.endTimeMs.has_value()) {
    std::vector<std::uint8_t> time; status = EncodeBinaryTimeValue(*request.endTimeMs, &time); if (!status.ok()) return status;
    status = Append(0x82, time, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end());
  }
  if (request.numberOfEntries.has_value()) {
    status = AppendUnsigned(0x83, *request.numberOfEntries, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end());
  }
  if (!request.variableTags.empty()) {
    std::vector<std::uint8_t> tags;
    for (const auto& tag : request.variableTags) { status = AppendString(0x1a, tag, &field); if (!status.ok()) return status; tags.insert(tags.end(), field.begin(), field.end()); }
    status = Append(0xa4, tags, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end());
  }
  if (request.startAfterTimeMs.has_value() || !request.startAfterEntryId.empty()) {
    std::vector<std::uint8_t> after;
    if (request.startAfterTimeMs.has_value()) { std::vector<std::uint8_t> time; status = EncodeBinaryTimeValue(*request.startAfterTimeMs, &time); if (!status.ok()) return status; status = Append(0x80, time, &field); if (!status.ok()) return status; after.insert(after.end(), field.begin(), field.end()); }
    if (!request.startAfterEntryId.empty()) { if (request.startAfterEntryId.size() > 8) return Argument("Journal续读EntryID过长"); status = Append(0x81, request.startAfterEntryId, &field); if (!status.ok()) return status; after.insert(after.end(), field.begin(), field.end()); }
    status = Append(0xa5, after, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end());
  }
  return EncodeMmsConfirmedRequest(invokeId, 65, content, output, outputSize);
}

grpc::Status DecodeMmsJournalReadRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsJournalReadRequest* request) {
  if (request == nullptr || invokeId == nullptr) return Argument("ReadJournal请求输出为空");
  *request = {};
  MmsConfirmedPduView pdu; auto status = DecodeMmsConfirmedRequest(input, &pdu);
  if (!status.ok() || pdu.serviceTag != 65) return Invalid("ReadJournal服务选择错误");
  std::size_t offset = 0; BerTlvView object; status = ReadBerTlv(pdu.serviceValue, &offset, &object); if (!status.ok()) return status;
  std::vector<std::uint8_t> objectEncoded; status = Append(object.tag, object.value, &objectEncoded); if (!status.ok()) return status; status = DecodeObject(objectEncoded, &request->journal); if (!status.ok()) return status;
  while (offset < pdu.serviceValue.size()) {
    BerTlvView field; status = ReadBerTlv(pdu.serviceValue, &offset, &field); if (!status.ok()) return status;
    if (field.tag == 0x80 || field.tag == 0x82) { std::int64_t time = 0; status = DecodeBinaryTimeValue(field.value, &time); if (!status.ok()) return status; (field.tag == 0x80 ? request->startTimeMs : request->endTimeMs) = time; }
    else if (field.tag == 0x81) { if (field.value.empty() || field.value.size() > 8) return Invalid("ReadJournal起始EntryID无效"); request->startEntryId.assign(field.value.begin(), field.value.end()); }
    else if (field.tag == 0x83) { std::uint64_t count = 0; if (!ReadBerUnsigned(field.value, &count).ok() || count > std::numeric_limits<std::uint32_t>::max()) return Invalid("ReadJournal数量无效"); request->numberOfEntries = static_cast<std::uint32_t>(count); }
    else if (field.tag == 0xa4 || field.tag == 0xa5) { std::size_t inner = 0; while (inner < field.value.size()) { BerTlvView item; status = ReadBerTlv(field.value, &inner, &item); if (!status.ok()) return status; if (field.tag == 0xa4) { if (item.tag != 0x1a || item.value.empty()) return Invalid("ReadJournal变量标签无效"); request->variableTags.emplace_back(reinterpret_cast<const char*>(item.value.data()), item.value.size()); } else if (item.tag == 0x80) { std::int64_t time = 0; status = DecodeBinaryTimeValue(item.value, &time); if (!status.ok()) return status; request->startAfterTimeMs = time; } else if (item.tag == 0x81) { if (item.value.empty() || item.value.size() > 8) return Invalid("ReadJournal续读EntryID无效"); request->startAfterEntryId.assign(item.value.begin(), item.value.end()); } else return Invalid("ReadJournal续读字段无效"); } }
    else return Invalid("ReadJournal包含未知字段");
  }
  *invokeId = pdu.invokeId; return grpc::Status::OK;
}

grpc::Status EncodeMmsJournalReadResponse(
    std::uint32_t invokeId, const MmsJournalReadResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (response.entries.size() > kMaxJournalEntries) return Resource("Journal条目数量超过上限");
  std::vector<std::uint8_t> entries;
  for (const auto& entry : response.entries) { std::vector<std::uint8_t> encoded; auto status = EncodeJournalEntry(entry, &encoded); if (!status.ok()) return status; entries.insert(entries.end(), encoded.begin(), encoded.end()); }
  std::vector<std::uint8_t> list; auto status = Append(0xa0, entries, &list); if (!status.ok()) return status;
  std::vector<std::uint8_t> content = list; if (response.moreFollows) { std::vector<std::uint8_t> more; status = Append(0x81, std::array<std::uint8_t, 1>{0xff}, &more); if (!status.ok()) return status; content.insert(content.end(), more.begin(), more.end()); }
  return EncodeMmsConfirmedResponse(invokeId, 65, content, output, outputSize);
}

grpc::Status DecodeMmsJournalReadResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsJournalReadResponse* response) {
  if (response == nullptr) return Argument("ReadJournal响应输出为空"); *response = {};
  MmsConfirmedPduView pdu; auto status = DecodeMmsConfirmedResponse(input, &pdu); if (!status.ok() || pdu.invokeId != expectedInvokeId || pdu.serviceTag != 65) return Invalid("ReadJournal响应服务选择错误");
  std::size_t offset = 0; BerTlvView list; status = ReadBerTlv(pdu.serviceValue, &offset, &list); if (!status.ok() || list.tag != 0xa0) return Invalid("ReadJournal响应条目列表错误");
  std::size_t inner = 0; while (inner < list.value.size()) { if (response->entries.size() >= kMaxJournalEntries) return Resource("Journal条目数量超过上限"); BerTlvView entry; status = ReadBerTlv(list.value, &inner, &entry); if (!status.ok()) return status; std::vector<std::uint8_t> encoded; status = Append(entry.tag, entry.value, &encoded); if (!status.ok()) return status; MmsJournalEntry decoded; status = DecodeJournalEntry(encoded, &decoded); if (!status.ok()) return status; response->entries.emplace_back(std::move(decoded)); }
  if (offset < pdu.serviceValue.size()) { BerTlvView more; status = ReadBerTlv(pdu.serviceValue, &offset, &more); bool value = false; if (!status.ok() || more.tag != 0x81 || !ReadBerBoolean(more.value, &value).ok()) return Invalid("ReadJournal moreFollows字段错误"); response->moreFollows = value; }
  if (offset != pdu.serviceValue.size() || (response->moreFollows && response->entries.empty())) return Invalid("ReadJournal分页未前进"); return grpc::Status::OK;
}

grpc::Status EncodeMmsDefineNamedVariableListRequest(
    std::uint32_t invokeId, const MmsNamedVariableListDefinition& request,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (request.variables.empty() || request.variables.size() > kMaxJournalVariables) return Argument("动态DataSet成员为空或超限");
  std::vector<std::uint8_t> name; auto status = EncodeObject(request.listName, &name); if (!status.ok()) return status;
  std::vector<std::uint8_t> vars;
  for (const auto& variable : request.variables) { std::vector<std::uint8_t> object; status = EncodeObject(variable, &object); if (!status.ok()) return status; std::vector<std::uint8_t> spec; status = Append(0xa0, object, &spec); if (!status.ok()) return status; std::vector<std::uint8_t> item; status = Append(0x30, spec, &item); if (!status.ok()) return status; vars.insert(vars.end(), item.begin(), item.end()); }
  std::vector<std::uint8_t> content = name; std::vector<std::uint8_t> list; status = Append(0xa0, vars, &list); if (!status.ok()) return status; content.insert(content.end(), list.begin(), list.end());
  return EncodeMmsConfirmedRequest(invokeId, 11, content, output, outputSize);
}

grpc::Status DecodeMmsDefineNamedVariableListRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsNamedVariableListDefinition* request) {
  if (request == nullptr || invokeId == nullptr) return Argument("动态DataSet请求输出为空"); *request = {};
  MmsConfirmedPduView pdu; auto status = DecodeMmsConfirmedRequest(input, &pdu); if (!status.ok() || pdu.serviceTag != 11) return Invalid("DefineNamedVariableList服务选择错误");
  std::size_t offset = 0; BerTlvView name; status = ReadBerTlv(pdu.serviceValue, &offset, &name); if (!status.ok()) return status; std::vector<std::uint8_t> nameEncoded; status = Append(name.tag, name.value, &nameEncoded); if (!status.ok()) return status; status = DecodeObject(nameEncoded, &request->listName); if (!status.ok()) return status;
  BerTlvView list; status = ReadBerTlv(pdu.serviceValue, &offset, &list); if (!status.ok() || list.tag != 0xa0 || offset != pdu.serviceValue.size()) return Invalid("动态DataSet成员列表错误");
  std::size_t inner = 0; while (inner < list.value.size()) { if (request->variables.size() >= kMaxJournalVariables) return Resource("动态DataSet成员超限"); BerTlvView item; status = ReadBerTlv(list.value, &inner, &item); if (!status.ok() || item.tag != 0x30) return Invalid("动态DataSet成员结构错误"); std::size_t itemOffset = 0; BerTlvView spec; status = ReadBerTlv(item.value, &itemOffset, &spec); if (!status.ok() || itemOffset != item.value.size() || spec.tag != 0xa0) return Invalid("动态DataSet成员访问选择错误"); std::vector<std::uint8_t> object(spec.value.begin(), spec.value.end()); MmsObjectName variable; status = DecodeObject(object, &variable); if (!status.ok()) return status; request->variables.emplace_back(std::move(variable)); }
  *invokeId = pdu.invokeId; return grpc::Status::OK;
}

grpc::Status EncodeMmsDeleteNamedVariableListRequest(
    std::uint32_t invokeId, const MmsObjectName& listName,
    std::span<std::uint8_t> output, std::size_t* outputSize) { return EncodeObjectRequest(invokeId, 13, listName, output, outputSize); }

grpc::Status DecodeMmsDeleteNamedVariableListRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsObjectName* listName) { return DecodeObjectRequest(input, 13, invokeId, listName); }

grpc::Status EncodeMmsInitiateDownloadRequest(std::uint32_t invokeId, const MmsInitiateDownloadRequest& request, std::span<std::uint8_t> output, std::size_t* outputSize) { if (request.domain.empty() || request.fileName.empty()) return Argument("InitiateDownload文件名为空"); std::vector<std::uint8_t> content, field; auto status = AppendString(0x80, request.domain, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end()); status = AppendString(0x81, request.fileName, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end()); status = AppendUnsigned(0x82, request.fileSize, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end()); return EncodeMmsConfirmedRequest(invokeId, 26, content, output, outputSize); }

grpc::Status DecodeMmsInitiateDownloadRequest(std::span<const std::uint8_t> input, std::uint32_t* invokeId, MmsInitiateDownloadRequest* request) { if (request == nullptr || invokeId == nullptr) return Argument("InitiateDownload请求输出为空"); *request = {}; MmsConfirmedPduView pdu; auto status = DecodeMmsConfirmedRequest(input, &pdu); if (!status.ok() || pdu.serviceTag != 26) return Invalid("InitiateDownload服务选择错误"); std::size_t offset = 0; BerTlvView field; status = ReadBerTlv(pdu.serviceValue, &offset, &field); if (!status.ok() || field.tag != 0x80) return Invalid("InitiateDownload域字段错误"); request->domain.assign(reinterpret_cast<const char*>(field.value.data()), field.value.size()); status = ReadBerTlv(pdu.serviceValue, &offset, &field); if (!status.ok() || field.tag != 0x81) return Invalid("InitiateDownload文件名字段错误"); request->fileName.assign(reinterpret_cast<const char*>(field.value.data()), field.value.size()); status = ReadBerTlv(pdu.serviceValue, &offset, &field); std::uint64_t size = 0; if (!status.ok() || field.tag != 0x82 || !ReadBerUnsigned(field.value, &size).ok() || offset != pdu.serviceValue.size()) return Invalid("InitiateDownload大小字段错误"); request->fileSize = size; *invokeId = pdu.invokeId; return grpc::Status::OK; }

grpc::Status DecodeMmsInitiateDownloadResponse(std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId, MmsInitiateDownloadResponse* response) { if (response == nullptr) return Argument("InitiateDownload响应输出为空"); *response = {}; MmsConfirmedPduView pdu; auto status = DecodeMmsConfirmedResponse(input, &pdu); if (!status.ok() || pdu.invokeId != expectedInvokeId || pdu.serviceTag != 26) return Invalid("InitiateDownload响应服务选择错误"); std::int64_t id = 0; if (!ReadBerSigned(pdu.serviceValue, &id).ok() || id < 0 || id > std::numeric_limits<std::int32_t>::max()) return Invalid("InitiateDownload句柄无效"); response->frsmId = static_cast<std::int32_t>(id); return grpc::Status::OK; }

grpc::Status EncodeMmsDownloadSegmentRequest(std::uint32_t invokeId, const MmsDownloadSegmentRequest& request, std::span<std::uint8_t> output, std::size_t* outputSize) { if (request.frsmId < 0 || request.data.empty() || request.data.size() > kMaxOptionalBytes) return Argument("DownloadSegment参数无效"); std::vector<std::uint8_t> content, field; auto status = AppendSigned(0x80, request.frsmId, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end()); status = Append(0x81, request.data, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end()); return EncodeMmsConfirmedRequest(invokeId, 27, content, output, outputSize); }

grpc::Status DecodeMmsDownloadSegmentRequest(std::span<const std::uint8_t> input, std::uint32_t* invokeId, MmsDownloadSegmentRequest* request) { if (request == nullptr || invokeId == nullptr) return Argument("DownloadSegment请求输出为空"); *request = {}; MmsConfirmedPduView pdu; auto status = DecodeMmsConfirmedRequest(input, &pdu); if (!status.ok() || pdu.serviceTag != 27) return Invalid("DownloadSegment服务选择错误"); std::size_t offset = 0; BerTlvView field; status = ReadBerTlv(pdu.serviceValue, &offset, &field); std::int64_t id = 0; if (!status.ok() || field.tag != 0x80 || !ReadBerSigned(field.value, &id).ok() || id < 0 || id > std::numeric_limits<std::int32_t>::max()) return Invalid("DownloadSegment句柄无效"); request->frsmId = static_cast<std::int32_t>(id); status = ReadBerTlv(pdu.serviceValue, &offset, &field); if (!status.ok() || field.tag != 0x81 || field.value.empty() || offset != pdu.serviceValue.size()) return Invalid("DownloadSegment数据字段错误"); request->data.assign(field.value.begin(), field.value.end()); *invokeId = pdu.invokeId; return grpc::Status::OK; }

grpc::Status EncodeMmsTerminateDownloadRequest(std::uint32_t invokeId, const MmsTerminateDownloadRequest& request, std::span<std::uint8_t> output, std::size_t* outputSize) { if (request.frsmId < 0) return Argument("TerminateDownload句柄无效"); std::vector<std::uint8_t> value; auto status = AppendSigned(0x80, request.frsmId, &value); if (!status.ok()) return status; return EncodeMmsConfirmedRequest(invokeId, 28, value, output, outputSize); }

grpc::Status DecodeMmsTerminateDownloadRequest(std::span<const std::uint8_t> input, std::uint32_t* invokeId, MmsTerminateDownloadRequest* request) { if (request == nullptr || invokeId == nullptr) return Argument("TerminateDownload请求输出为空"); *request = {}; MmsConfirmedPduView pdu; auto status = DecodeMmsConfirmedRequest(input, &pdu); if (!status.ok() || pdu.serviceTag != 28) return Invalid("TerminateDownload服务选择错误"); std::int64_t id = 0; std::size_t offset = 0; BerTlvView field; if (ReadBerTlv(pdu.serviceValue, &offset, &field).ok() && offset == pdu.serviceValue.size() && field.tag == 0x80) { status = ReadBerSigned(field.value, &id); } else { status = ReadBerSigned(pdu.serviceValue, &id); } if (!status.ok() || id < 0 || id > std::numeric_limits<std::int32_t>::max()) return Invalid("TerminateDownload句柄无效"); request->frsmId = static_cast<std::int32_t>(id); *invokeId = pdu.invokeId; return grpc::Status::OK; }

grpc::Status EncodeMmsFileDeleteRequest(std::uint32_t invokeId, std::string_view fileName, std::span<std::uint8_t> output, std::size_t* outputSize) { if (fileName.empty()) return Argument("FileDelete文件名为空"); std::vector<std::uint8_t> name; auto status = AppendString(0x19, fileName, &name); if (!status.ok()) return status; return EncodeMmsConfirmedRequest(invokeId, 76, name, output, outputSize); }

grpc::Status EncodeMmsFileRenameRequest(std::uint32_t invokeId, std::string_view currentName, std::string_view newName, std::span<std::uint8_t> output, std::size_t* outputSize) { if (currentName.empty() || newName.empty()) return Argument("FileRename文件名为空"); std::vector<std::uint8_t> content, field; auto status = AppendString(0x19, currentName, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end()); status = AppendString(0x19, newName, &field); if (!status.ok()) return status; content.insert(content.end(), field.begin(), field.end()); return EncodeMmsConfirmedRequest(invokeId, 75, content, output, outputSize); }

}  // namespace IEC61850
