#include "IEC61850MmsRcb.h"

#include <array>
#include <format>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "IEC61850MmsBer.h"
#include "IEC61850MmsSession.h"
#include "IEC61850MmsService.h"

namespace IEC61850 {
namespace {

struct DataField {
  std::uint32_t tag = 0;
  std::span<const std::uint8_t> value;
};

grpc::Status InvalidField(std::string_view name, std::string_view reason) {
  return grpc::Status(
      grpc::StatusCode::DATA_LOSS,
      std::format("IEC61850 MMS RCB字段{}无效: {}", name, reason));
}

grpc::Status DecodeStructure(std::span<const std::uint8_t> encoded,
                             std::vector<DataField>* fields) {
  if (fields == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS RCB结构输出参数为空");
  }
  fields->clear();
  std::size_t offset = 0;
  BerTlvView structure;
  auto status = ReadBerTlv(encoded, &offset, &structure);
  if (!status.ok() || offset != encoded.size() || structure.tag != 0xa2) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "IEC61850 MMS RCB根对象不是Data.structure");
  }
  offset = 0;
  while (offset < structure.value.size()) {
    BerTlvView field;
    status = ReadBerTlv(structure.value, &offset, &field);
    if (!status.ok()) {
      return status;
    }
    if (fields->size() >= 64) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "IEC61850 MMS RCB结构字段超过下位机上限");
    }
    fields->push_back({field.tag, field.value});
  }
  return grpc::Status::OK;
}

grpc::Status RequireField(const std::vector<DataField>& fields,
                          std::size_t index, std::uint8_t tag,
                          std::string_view name,
                          std::span<const std::uint8_t>* value) {
  if (value == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS RCB字段值输出参数为空");
  }
  *value = {};
  if (index >= fields.size()) {
    return InvalidField(name, "结构成员缺失");
  }
  if (fields[index].tag != tag) {
    return InvalidField(name,
                       std::format("标签为0x{:02x}，期望0x{:02x}",
                                   fields[index].tag, tag));
  }
  *value = fields[index].value;
  return grpc::Status::OK;
}

grpc::Status ReadVisible(const std::vector<DataField>& fields,
                         std::size_t index, std::string_view name,
                         bool requireNonEmpty, std::string* value) {
  if (value == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS RCB VisibleString输出参数为空");
  }
  std::span<const std::uint8_t> raw;
  auto status = RequireField(fields, index, 0x8a, name, &raw);
  if (!status.ok()) {
    return status;
  }
  if (requireNonEmpty && raw.empty()) {
    return InvalidField(name, "VisibleString为空");
  }
  value->clear();
  if (!raw.empty()) {
    value->assign(reinterpret_cast<const char*>(raw.data()), raw.size());
  }
  return grpc::Status::OK;
}

grpc::Status ReadBoolean(const std::vector<DataField>& fields,
                         std::size_t index, std::string_view name,
                         bool* value) {
  std::span<const std::uint8_t> raw;
  auto status = RequireField(fields, index, 0x83, name, &raw);
  if (!status.ok()) {
    return status;
  }
  status = ReadBerBoolean(raw, value);
  return status.ok() ? status : InvalidField(name, "BOOLEAN编码错误");
}

grpc::Status ReadUnsigned(const std::vector<DataField>& fields,
                          std::size_t index, std::string_view name,
                          std::uint64_t* value) {
  std::span<const std::uint8_t> raw;
  auto status = RequireField(fields, index, 0x86, name, &raw);
  if (!status.ok()) {
    return status;
  }
  status = ReadBerUnsigned(raw, value);
  return status.ok() ? status : InvalidField(name, "Unsigned编码错误");
}

struct BitStringView {
  std::span<const std::uint8_t> bytes;
  std::size_t bitCount = 0;
};

grpc::Status ReadBitString(const std::vector<DataField>& fields,
                           std::size_t index, std::string_view name,
                           std::size_t requiredBits, BitStringView* value) {
  if (value == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS RCB位串输出参数为空");
  }
  *value = {};
  std::span<const std::uint8_t> raw;
  auto status = RequireField(fields, index, 0x84, name, &raw);
  if (!status.ok()) {
    return status;
  }
  if (raw.empty() || raw.front() > 7) {
    return InvalidField(name, "BIT STRING未使用位数错误");
  }
  const auto unused = static_cast<std::size_t>(raw.front());
  const auto payload = raw.subspan(1);
  if (payload.empty() || unused > payload.size() * 8) {
    return InvalidField(name, "BIT STRING长度错误");
  }
  if (unused != 0 &&
      (payload.back() & static_cast<std::uint8_t>((1u << unused) - 1u)) != 0) {
    return InvalidField(name, "BIT STRING未使用低位非零");
  }
  value->bytes = payload;
  value->bitCount = payload.size() * 8 - unused;
  if (value->bitCount != requiredBits) {
    return InvalidField(name, "BIT STRING有效位数不匹配");
  }
  return grpc::Status::OK;
}

bool BitAt(const BitStringView& value, std::size_t index) noexcept {
  return index < value.bitCount &&
         (value.bytes[index / 8] &
          static_cast<std::uint8_t>(0x80u >> (index % 8))) != 0;
}

grpc::Status BuildMemberObject(std::string_view rcbRef,
                               std::string_view member,
                               MmsObjectName* objectName) {
  if (objectName == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS RCB对象名输出参数为空");
  }
  auto status = ParseMmsDomainObjectReference(rcbRef, objectName);
  if (!status.ok()) {
    return status;
  }
  if (member.empty() || member.find('/') != std::string_view::npos ||
      member.find('$') != std::string_view::npos) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS RCB属性名无效");
  }
  objectName->identifier.append("$");
  objectName->identifier.append(member);
  return grpc::Status::OK;
}

grpc::Status AddBooleanItem(const MmsRcbActivationRequest& activation,
                            std::string_view member, bool value,
                            MmsWriteRequest* request) {
  MmsWriteRequestItem item;
  auto status = BuildMemberObject(activation.rcbRef, member, &item.variable);
  if (!status.ok()) {
    return status;
  }
  status = EncodeMmsDataBoolean(value, &item.encodedData);
  if (!status.ok()) {
    return status;
  }
  request->items.emplace_back(std::move(item));
  return grpc::Status::OK;
}

grpc::Status AddVisibleStringItem(const MmsRcbActivationRequest& activation,
                                  std::string_view member,
                                  std::string_view value,
                                  MmsWriteRequest* request) {
  MmsWriteRequestItem item;
  auto status = BuildMemberObject(activation.rcbRef, member, &item.variable);
  if (!status.ok()) {
    return status;
  }
  status = EncodeMmsDataVisibleString(value, &item.encodedData);
  if (!status.ok()) {
    return status;
  }
  request->items.emplace_back(std::move(item));
  return grpc::Status::OK;
}

grpc::Status AddUnsignedItem(const MmsRcbActivationRequest& activation,
                             std::string_view member, std::uint64_t value,
                             MmsWriteRequest* request) {
  MmsWriteRequestItem item;
  auto status = BuildMemberObject(activation.rcbRef, member, &item.variable);
  if (!status.ok()) {
    return status;
  }
  status = EncodeMmsDataUnsigned(value, &item.encodedData);
  if (!status.ok()) {
    return status;
  }
  request->items.emplace_back(std::move(item));
  return grpc::Status::OK;
}

grpc::Status AddBitStringItem(const MmsRcbActivationRequest& activation,
                              std::string_view member, std::uint8_t unusedBits,
                              std::span<const std::uint8_t> payload,
                              MmsWriteRequest* request) {
  MmsWriteRequestItem item;
  auto status = BuildMemberObject(activation.rcbRef, member, &item.variable);
  if (!status.ok()) {
    return status;
  }
  status = EncodeMmsDataBitString(unusedBits, payload, &item.encodedData);
  if (!status.ok()) {
    return status;
  }
  request->items.emplace_back(std::move(item));
  return grpc::Status::OK;
}

std::array<std::uint8_t, 2> EncodeOptionalFields(
    const IEC61850Proto::SclOptionalFields& fields) noexcept {
  std::array<std::uint8_t, 2> payload{};
  const bool values[] = {
      false,
      fields.sequence_number(),
      fields.report_timestamp(),
      fields.reason_code(),
      fields.data_set(),
      fields.data_reference(),
      fields.buffer_overflow(),
      fields.entry_id(),
      fields.config_revision(),
      fields.segmentation(),
  };
  for (std::size_t index = 0; index < std::size(values); ++index) {
    if (values[index]) {
      payload[index / 8] = static_cast<std::uint8_t>(
          payload[index / 8] | (0x80u >> (index % 8)));
    }
  }
  return payload;
}

std::array<std::uint8_t, 1> EncodeTriggerOptions(
    const IEC61850Proto::SclTriggerOptions& options) noexcept {
  std::array<std::uint8_t, 1> payload{};
  const bool values[] = {
      false,
      options.data_change(),
      options.quality_change(),
      options.data_update(),
      options.integrity(),
      options.general_interrogation(),
  };
  for (std::size_t index = 0; index < std::size(values); ++index) {
    if (values[index]) {
      payload[0] = static_cast<std::uint8_t>(
          payload[0] | (0x80u >> (index % 8)));
    }
  }
  return payload;
}

}  // namespace

grpc::Status BuildMmsRcbWriteRequest(
    const MmsRcbActivationRequest& activation, MmsRcbWritePhase phase,
    MmsWriteRequest* request) {
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS RCB Write请求输出参数为空");
  }
  *request = {};
  if (activation.rcbRef.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS RCB引用不能为空");
  }
  MmsObjectName base;
  auto status = ParseMmsDomainObjectReference(activation.rcbRef, &base);
  if (!status.ok()) {
    return status;
  }
  (void)base;

  switch (phase) {
    case MmsRcbWritePhase::DISABLE:
      return AddBooleanItem(activation, "RptEna", false, request);
    case MmsRcbWritePhase::ENABLE:
      return AddBooleanItem(activation, "RptEna", true, request);
    case MmsRcbWritePhase::CONFIGURE:
      break;
    default:
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "IEC61850 MMS RCB Write阶段无效");
  }
  if (activation.reportId.empty() || activation.dataSetRef.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS RCB配置缺少ReportID或DataSet");
  }
  status = AddVisibleStringItem(activation, "RptID", activation.reportId,
                                request);
  if (!status.ok()) {
    return status;
  }
  status = AddVisibleStringItem(activation, "DatSet", activation.dataSetRef,
                                request);
  if (!status.ok()) {
    return status;
  }
  const auto optionalFields = EncodeOptionalFields(activation.optionalFields);
  status = AddBitStringItem(activation, "OptFlds", 6, optionalFields, request);
  if (!status.ok()) {
    return status;
  }
  status = AddUnsignedItem(activation, "BufTm", activation.bufferTimeMs,
                           request);
  if (!status.ok()) {
    return status;
  }
  const auto triggerOptions = EncodeTriggerOptions(activation.triggerOptions);
  status = AddBitStringItem(activation, "TrgOps", 2, triggerOptions, request);
  if (!status.ok()) {
    return status;
  }
  return AddUnsignedItem(activation, "IntgPd", activation.integrityPeriodMs,
                         request);
}

grpc::Status BuildMmsRcbGeneralInterrogationRequest(
    const MmsRcbActivationRequest& activation, MmsWriteRequest* request) {
  if (request == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS GI Write请求输出参数为空");
  }
  *request = {};
  if (activation.rcbRef.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS GI请求RCB引用不能为空");
  }
  if (!activation.generalInterrogation) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS当前RCB未声明GI触发条件");
  }
  return AddBooleanItem(activation, "GI", true, request);
}

grpc::Status DecodeMmsRcbData(std::span<const std::uint8_t> encodedData,
                              bool buffered,
                              MmsDirectoryReportControl* result) {
  if (result == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS RCB输出参数为空");
  }
  *result = {};
  std::vector<DataField> fields;
  auto status = DecodeStructure(encodedData, &fields);
  if (!status.ok()) {
    return status;
  }

  // BRCB在RptEna后直接是DatSet；URCB多一个Resv布尔字段。
  const std::size_t dataSetIndex = buffered ? 2 : 3;
  const std::size_t confRevIndex = buffered ? 3 : 4;
  const std::size_t optFldsIndex = buffered ? 4 : 5;
  const std::size_t bufTmIndex = buffered ? 5 : 6;
  const std::size_t sqNumIndex = buffered ? 6 : 7;
  const std::size_t trgOpsIndex = buffered ? 7 : 8;
  const std::size_t intgPdIndex = buffered ? 8 : 9;
  const std::size_t giIndex = buffered ? 9 : 10;

  status = ReadVisible(fields, 0, "RptID", false, &result->reportId);
  if (!status.ok()) {
    return status;
  }
  status = ReadBoolean(fields, 1, "RptEna", &result->reportEnabled);
  if (!status.ok()) {
    return status;
  }
  if (!buffered) {
    bool reserved = false;
    status = ReadBoolean(fields, 2, "Resv", &reserved);
    if (!status.ok()) {
      return status;
    }
  }
  status = ReadVisible(fields, dataSetIndex, "DatSet", true,
                       &result->dataSetRef);
  if (!status.ok()) {
    return status;
  }
  status = ReadUnsigned(fields, confRevIndex, "ConfRev",
                        &result->configRevision);
  if (!status.ok()) {
    return status;
  }
  BitStringView optionalFields;
  status = ReadBitString(fields, optFldsIndex, "OptFlds", 10,
                         &optionalFields);
  if (!status.ok()) {
    return status;
  }
  if (BitAt(optionalFields, 0)) {
    return InvalidField("OptFlds", "保留位必须为零");
  }
  // OptFlds bit0为保留位，SCL字段从bit1开始。
  result->optionalFields.set_sequence_number(BitAt(optionalFields, 1));
  result->optionalFields.set_report_timestamp(BitAt(optionalFields, 2));
  result->optionalFields.set_reason_code(BitAt(optionalFields, 3));
  result->optionalFields.set_data_set(BitAt(optionalFields, 4));
  result->optionalFields.set_data_reference(BitAt(optionalFields, 5));
  result->optionalFields.set_buffer_overflow(BitAt(optionalFields, 6));
  result->optionalFields.set_entry_id(BitAt(optionalFields, 7));
  result->optionalFields.set_config_revision(BitAt(optionalFields, 8));
  result->optionalFields.set_segmentation(BitAt(optionalFields, 9));

  std::uint64_t numeric = 0;
  status = ReadUnsigned(fields, bufTmIndex, "BufTm", &numeric);
  if (!status.ok() || numeric > std::numeric_limits<std::uint32_t>::max()) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "IEC61850 MMS RCB BufTm超出下位机范围");
  }
  result->bufferTimeMs = static_cast<std::uint32_t>(numeric);
  status = ReadUnsigned(fields, sqNumIndex, "SqNum", &numeric);
  if (!status.ok()) {
    return status;
  }
  BitStringView triggerOptions;
  status = ReadBitString(fields, trgOpsIndex, "TrgOps", 6,
                         &triggerOptions);
  if (!status.ok()) {
    return status;
  }
  if (BitAt(triggerOptions, 0)) {
    return InvalidField("TrgOps", "保留位必须为零");
  }
  result->triggerOptions.set_data_change(BitAt(triggerOptions, 1));
  result->triggerOptions.set_quality_change(BitAt(triggerOptions, 2));
  result->triggerOptions.set_data_update(BitAt(triggerOptions, 3));
  result->triggerOptions.set_integrity(BitAt(triggerOptions, 4));
  result->triggerOptions.set_general_interrogation(BitAt(triggerOptions, 5));
  status = ReadUnsigned(fields, intgPdIndex, "IntgPd", &numeric);
  if (!status.ok() || numeric > std::numeric_limits<std::uint32_t>::max()) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "IEC61850 MMS RCB IntgPd超出下位机范围");
  }
  result->integrityPeriodMs = static_cast<std::uint32_t>(numeric);
  bool generalInterrogation = false;
  status = ReadBoolean(fields, giIndex, "GI", &generalInterrogation);
  if (!status.ok()) {
    return status;
  }
  (void)generalInterrogation;
  // RCB根对象的GI是当前值核对字段；结果结构不向上层暴露GI状态，
  // 但必须保留RptEna的真实值，不能把GI误写入reportEnabled。
  for (std::size_t index = giIndex + 1; index < fields.size(); ++index) {
    switch (fields[index].tag) {
      case 0x83:  // PurgeBuf/Resv等BOOLEAN可选成员。
      case 0x85:  // 旧设备将保留时长编码为Integer。
      case 0x86:  // ResvTms等Unsigned可选成员。
      case 0x89:  // EntryID/Owner等OctetString可选成员。
      case 0x8a:  // 少数设备将Owner作为VisibleString返回。
      case 0x8c:  // TimeOfEntry BinaryTime可选成员。
      case 0x8e:  // 兼容少数设备的扩展时间选择。
        break;
      default:
        return InvalidField("RCB尾部", "包含未知Data选择");
    }
  }
  return grpc::Status::OK;
}

}  // namespace IEC61850
