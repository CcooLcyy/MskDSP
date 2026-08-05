#include "IEC61850MmsService.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <string_view>
#include <utility>

#include "IEC61850MmsBer.h"

namespace IEC61850 {
namespace {

constexpr std::size_t kMaxMmsPduBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxIdentifierBytes = 1024;
constexpr std::size_t kMaxNameListEntries = 4096;
constexpr std::size_t kMaxTypeSpecificationDepth = 32;
constexpr std::size_t kMaxTypeSpecificationComponents = 4096;
constexpr std::uint64_t kMaxTypeWidth = 1024 * 1024;
constexpr std::int64_t kMillisecondsPerDay = 86400000;
// IEC 61850 BinaryTime(6)的日期基准为1984-01-01，距Unix epoch共5113天。
constexpr std::int64_t kDaysFrom1984To1970 = 5113;

grpc::Status Invalid(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::DATA_LOSS,
                      std::format("IEC61850 MMS服务报文无效: {}", reason));
}

grpc::Status ArgumentError(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      std::format("IEC61850 MMS服务参数无效: {}", reason));
}

grpc::Status OutputError(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                      std::format("IEC61850 MMS服务输出缓冲不足: {}", reason));
}

bool IsContextConstructed(std::uint8_t tag, std::uint8_t number) noexcept {
  return tag == static_cast<std::uint8_t>(0xa0 | number);
}

bool IsContextPrimitive(std::uint8_t tag, std::uint8_t number) noexcept {
  return tag == static_cast<std::uint8_t>(0x80 | number);
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

grpc::Status AppendTlv(std::uint32_t tag, std::span<const std::uint8_t> value,
                       std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return ArgumentError("BER字段输出参数为空");
  }
  if (value.size() > kMaxMmsPduBytes) {
    return OutputError("BER字段超过MMS上限");
  }
  output->assign(value.size() + sizeof(std::size_t) + 2, 0);
  BerWriter writer(*output);
  if (!writer.Tlv(tag, value)) {
    output->clear();
    return OutputError("BER字段编码失败");
  }
  output->resize(writer.size());
  return grpc::Status::OK;
}

grpc::Status AppendUnsigned(std::uint8_t tag, std::uint64_t value,
                            std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return ArgumentError("BER整数输出参数为空");
  }
  output->assign(sizeof(value) + 4, 0);
  BerWriter writer(*output);
  if (!writer.Unsigned(tag, value)) {
    output->clear();
    return OutputError("BER无符号整数编码失败");
  }
  output->resize(writer.size());
  return grpc::Status::OK;
}

grpc::Status AppendSigned(std::uint8_t tag, std::int64_t value,
                           std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return ArgumentError("BER有符号整数输出参数为空");
  }
  output->assign(sizeof(value) + 3, 0);
  BerWriter writer(*output);
  if (!writer.Signed(tag, value)) {
    output->clear();
    return OutputError("BER有符号整数编码失败");
  }
  output->resize(writer.size());
  return grpc::Status::OK;
}

grpc::Status ValidateReadResponseItem(
    bool success, std::span<const std::uint8_t> encoded) {
  if (encoded.empty()) {
    return ArgumentError(success ? "Read成功项缺少Data"
                                 : "Read失败项缺少DataAccessError");
  }
  std::size_t offset = 0;
  BerTlvView item;
  const auto status = ReadBerTlv(encoded, &offset, &item);
  if (!status.ok() || offset != encoded.size()) {
    return ArgumentError("Read响应项必须是完整单个BER TLV");
  }
  if (success) {
    return IsMmsDataTag(item.tag)
               ? grpc::Status::OK
               : ArgumentError("Read成功项不是合法MMS Data选择");
  }
  if (item.tag != 0x80) {
    return ArgumentError("Read失败项不是DataAccessError选择");
  }
  std::int64_t failureCode = 0;
  if (!ReadBerSigned(item.value, &failureCode).ok() || failureCode < 0 ||
      failureCode > 11) {
    return ArgumentError("Read失败项DataAccessError超出范围");
  }
  return grpc::Status::OK;
}

grpc::Status AppendIdentifier(std::string_view identifier,
                              std::vector<std::uint8_t>* output) {
  if (identifier.empty() || identifier.size() > kMaxIdentifierBytes) {
    return ArgumentError("MMS Identifier为空或超过长度上限");
  }
  return AppendTlv(
      0x1a,
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(identifier.data()),
          identifier.size()),
      output);
}

grpc::Status AppendContextIdentifier(std::uint8_t tag,
                                     std::string_view identifier,
                                     std::vector<std::uint8_t>* output) {
  if (identifier.empty() || identifier.size() > kMaxIdentifierBytes) {
    return ArgumentError("MMS Identifier为空或超过长度上限");
  }
  return AppendTlv(
      tag,
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(identifier.data()),
          identifier.size()),
      output);
}

grpc::Status CopyOutput(std::span<const std::uint8_t> encoded,
                        std::span<std::uint8_t> output,
                        std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return ArgumentError("MMS服务输出长度参数为空");
  }
  *outputSize = 0;
  if (encoded.size() > output.size()) {
    return OutputError("MMS服务PDU");
  }
  std::copy(encoded.begin(), encoded.end(), output.begin());
  *outputSize = encoded.size();
  return grpc::Status::OK;
}

grpc::Status EncodeConfirmed(std::uint8_t outerTag, std::uint32_t invokeId,
                             std::uint32_t serviceTag,
                             std::span<const std::uint8_t> serviceValue,
                             bool serviceConstructed,
                             std::span<std::uint8_t> output,
                             std::size_t* outputSize) {
  if (serviceTag == 0 || serviceTag > 0x1fffffffU) {
    return ArgumentError("确认服务选择标签超出上下文范围");
  }
  std::vector<std::uint8_t> invoke;
  auto status = AppendUnsigned(0x02, invokeId, &invoke);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> service;
  const auto classAndForm = serviceConstructed ? 0xbfu : 0x9fu;
  const auto encodedTag = serviceTag < 31
                              ? static_cast<std::uint32_t>(
                                    (serviceConstructed ? 0xa0 : 0x80) |
                                    serviceTag)
                              : (classAndForm << 24) | serviceTag;
  status = AppendTlv(encodedTag, serviceValue, &service);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> content;
  content.reserve(invoke.size() + service.size());
  content.insert(content.end(), invoke.begin(), invoke.end());
  content.insert(content.end(), service.begin(), service.end());
  std::vector<std::uint8_t> encoded;
  status = AppendTlv(outerTag, content, &encoded);
  if (!status.ok()) {
    return status;
  }
  return CopyOutput(encoded, output, outputSize);
}

grpc::Status DecodeConfirmed(std::span<const std::uint8_t> input,
                             std::uint8_t expectedOuter,
                             MmsConfirmedPduView* pdu) {
  if (pdu == nullptr) {
    return ArgumentError("Confirmed PDU输出参数为空");
  }
  *pdu = {};
  std::size_t offset = 0;
  BerTlvView outer;
  auto status = ReadBerTlv(input, &offset, &outer);
  if (!status.ok() || offset != input.size() || outer.tag != expectedOuter) {
    return Invalid("Confirmed PDU外层标签或长度错误");
  }
  std::size_t innerOffset = 0;
  BerTlvView invoke;
  status = ReadBerTlv(outer.value, &innerOffset, &invoke);
  if (!status.ok() || invoke.tag != 0x02) {
    return Invalid("Confirmed PDU缺少invokeID");
  }
  std::uint64_t invokeId = 0;
  status = ReadBerUnsigned(invoke.value, &invokeId);
  if (!status.ok() || invokeId > std::numeric_limits<std::uint32_t>::max()) {
    return Invalid("Confirmed PDU invokeID超出范围");
  }
  BerTlvView service;
  status = ReadBerTlv(outer.value, &innerOffset, &service);
  if (!status.ok() || innerOffset != outer.value.size() ||
      (service.identifierOctet & 0xc0) != 0x80 ||
      service.tagNumber == 0) {
    return Invalid("Confirmed PDU服务选择错误");
  }
  pdu->invokeId = static_cast<std::uint32_t>(invokeId);
  pdu->serviceTag = service.tagNumber;
  pdu->serviceValue = service.value;
  return grpc::Status::OK;
}

grpc::Status EncodeObjectScope(const MmsObjectScope& scope,
                               std::vector<std::uint8_t>* output) {
  std::vector<std::uint8_t> choice;
  grpc::Status status;
  switch (scope.type) {
    case MmsObjectScopeType::VMD_SPECIFIC:
      status = AppendTlv(0x80, {}, &choice);
      break;
    case MmsObjectScopeType::DOMAIN_SPECIFIC:
      if (scope.domain.empty() || scope.domain.size() > kMaxIdentifierBytes) {
        return ArgumentError("MMS Domain范围Identifier为空或过长");
      }
      status = AppendTlv(
          0x81,
          std::span<const std::uint8_t>(
              reinterpret_cast<const std::uint8_t*>(scope.domain.data()),
              scope.domain.size()),
          &choice);
      break;
    case MmsObjectScopeType::AA_SPECIFIC:
      status = AppendTlv(0x82, {}, &choice);
      break;
    default:
      return ArgumentError("MMS对象范围类型未知");
  }
  if (!status.ok()) {
    return status;
  }
  *output = std::move(choice);
  return grpc::Status::OK;
}

bool IsSupportedObjectClass(std::int64_t value) noexcept {
  return value == static_cast<std::int64_t>(MmsObjectClass::NAMED_VARIABLE) ||
         value ==
             static_cast<std::int64_t>(MmsObjectClass::NAMED_VARIABLE_LIST) ||
         value == static_cast<std::int64_t>(MmsObjectClass::NAMED_TYPE) ||
         value == static_cast<std::int64_t>(MmsObjectClass::DOMAIN);
}

grpc::Status DecodeObjectScope(std::span<const std::uint8_t> input,
                               MmsObjectScope* scope) {
  if (scope == nullptr) {
    return ArgumentError("MMS对象范围输出参数为空");
  }
  *scope = {};
  std::size_t offset = 0;
  BerTlvView choice;
  auto status = ReadBerTlv(input, &offset, &choice);
  if (!status.ok() || offset != input.size()) {
    return Invalid("MMS对象范围结构错误");
  }
  if (IsContextPrimitive(choice.tag, 0) && choice.value.empty()) {
    scope->type = MmsObjectScopeType::VMD_SPECIFIC;
    return grpc::Status::OK;
  }
  if (IsContextPrimitive(choice.tag, 1)) {
    if (choice.value.empty() || choice.value.size() > kMaxIdentifierBytes) {
      return Invalid("MMS Domain范围Identifier错误");
    }
    scope->type = MmsObjectScopeType::DOMAIN_SPECIFIC;
    scope->domain.assign(reinterpret_cast<const char*>(choice.value.data()),
                         choice.value.size());
    return grpc::Status::OK;
  }
  if (IsContextPrimitive(choice.tag, 2) && choice.value.empty()) {
    scope->type = MmsObjectScopeType::AA_SPECIFIC;
    return grpc::Status::OK;
  }
  return Invalid("MMS对象范围选择错误");
}

grpc::Status EncodeObjectName(const MmsObjectName& name,
                              std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return ArgumentError("MMS对象名输出参数为空");
  }
  if (name.identifier.empty() || name.identifier.size() > kMaxIdentifierBytes) {
    return ArgumentError("MMS对象名Identifier为空或过长");
  }
  std::vector<std::uint8_t> value;
  grpc::Status status;
  switch (name.type) {
    case MmsObjectNameType::VMD_SPECIFIC:
      status = AppendContextIdentifier(0x80, name.identifier, &value);
      break;
    case MmsObjectNameType::DOMAIN_SPECIFIC: {
      if (name.domain.empty() || name.domain.size() > kMaxIdentifierBytes) {
        return ArgumentError("MMS Domain对象名域标识为空或过长");
      }
      std::vector<std::uint8_t> item;
      status = AppendIdentifier(name.identifier, &item);
      if (!status.ok()) {
        return status;
      }
      std::vector<std::uint8_t> domain;
      status = AppendIdentifier(name.domain, &domain);
      if (!status.ok()) {
        return status;
      }
      value.reserve(item.size() + domain.size());
      value.insert(value.end(), domain.begin(), domain.end());
      value.insert(value.end(), item.begin(), item.end());
      std::vector<std::uint8_t> wrapped;
      status = AppendTlv(0xa1, value, &wrapped);
      value = std::move(wrapped);
      break;
    }
    case MmsObjectNameType::AA_SPECIFIC:
      status = AppendContextIdentifier(0x82, name.identifier, &value);
      break;
    default:
      return ArgumentError("MMS对象名类型未知");
  }
  if (!status.ok()) {
    return status;
  }
  *output = std::move(value);
  return grpc::Status::OK;
}

grpc::Status DecodeObjectName(std::span<const std::uint8_t> input,
                              MmsObjectName* name) {
  if (name == nullptr) {
    return ArgumentError("MMS对象名输出参数为空");
  }
  *name = {};
  std::size_t offset = 0;
  BerTlvView choice;
  auto status = ReadBerTlv(input, &offset, &choice);
  if (!status.ok() || offset != input.size()) {
    return Invalid("MMS对象名选择结构错误");
  }
  const auto copyIdentifier = [](std::span<const std::uint8_t> value,
                                 std::string* output) -> grpc::Status {
    if (output == nullptr || value.empty() ||
        value.size() > kMaxIdentifierBytes) {
      return Invalid("MMS对象名Identifier无效");
    }
    output->assign(reinterpret_cast<const char*>(value.data()), value.size());
    return grpc::Status::OK;
  };
  if (IsContextPrimitive(choice.tag, 0)) {
    name->type = MmsObjectNameType::VMD_SPECIFIC;
    return copyIdentifier(choice.value, &name->identifier);
  }
  if (IsContextPrimitive(choice.tag, 2)) {
    name->type = MmsObjectNameType::AA_SPECIFIC;
    return copyIdentifier(choice.value, &name->identifier);
  }
  if (!IsContextConstructed(choice.tag, 1)) {
    return Invalid("MMS对象名选择不受支持");
  }
  std::size_t nestedOffset = 0;
  BerTlvView domain;
  BerTlvView identifier;
  status = ReadBerTlv(choice.value, &nestedOffset, &domain);
  if (!status.ok() || domain.tag != 0x1a) {
    return Invalid("MMS Domain对象名缺少Domain标识");
  }
  status = ReadBerTlv(choice.value, &nestedOffset, &identifier);
  if (!status.ok() || nestedOffset != choice.value.size() ||
      identifier.tag != 0x1a) {
    return Invalid("MMS Domain对象名缺少Item标识");
  }
  name->type = MmsObjectNameType::DOMAIN_SPECIFIC;
  status = copyIdentifier(domain.value, &name->domain);
  if (!status.ok()) {
    return status;
  }
  return copyIdentifier(identifier.value, &name->identifier);
}

grpc::Status EncodeIntegerContent(bool signedValue, std::int64_t signedInteger,
                                  std::uint64_t unsignedInteger,
                                  std::vector<std::uint8_t>* output) {
  std::vector<std::uint8_t> encoded;
  auto status = signedValue
                    ? AppendSigned(0x02, signedInteger, &encoded)
                    : AppendUnsigned(0x02, unsignedInteger, &encoded);
  if (!status.ok()) {
    return status;
  }
  std::size_t offset = 0;
  BerTlvView integer;
  status = ReadBerTlv(encoded, &offset, &integer);
  if (!status.ok() || offset != encoded.size() || integer.tag != 0x02) {
    return Invalid("MMS文件整数内容编码失败");
  }
  output->assign(integer.value.begin(), integer.value.end());
  return grpc::Status::OK;
}

grpc::Status AppendGraphicString(std::string_view value,
                                 std::vector<std::uint8_t>* output) {
  if (value.empty() || value.size() > kMaxIdentifierBytes) {
    return ArgumentError("MMS文件名为空或超过长度上限");
  }
  return AppendTlv(
      0x19,
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(value.data()), value.size()),
      output);
}

grpc::Status AppendFileNameList(std::string_view value,
                                std::vector<std::uint8_t>* output) {
  // AR502H实际调用链传递单个远端文件名；仍以FileName的SEQUENCE OF
  // GraphicString形式封装，后续可在API层扩展多组件路径。
  return AppendGraphicString(value, output);
}

grpc::Status DecodeFileNameList(std::span<const std::uint8_t> input,
                                std::string* output) {
  if (output == nullptr) {
    return ArgumentError("MMS文件名输出参数为空");
  }
  output->clear();
  std::size_t offset = 0;
  std::size_t count = 0;
  while (offset < input.size()) {
    BerTlvView component;
    auto status = ReadBerTlv(input, &offset, &component);
    if (!status.ok() || component.tag != 0x19 || component.value.empty() ||
        component.value.size() > kMaxIdentifierBytes) {
      return Invalid("MMS文件名GraphicString无效");
    }
    if (count++ != 0) {
      output->push_back('/');
    }
    output->append(reinterpret_cast<const char*>(component.value.data()),
                   component.value.size());
    if (output->size() > kMaxMmsPduBytes) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "MMS文件名超过下位机上限");
    }
  }
  return count == 0 ? Invalid("MMS文件名序列为空") : grpc::Status::OK;
}

grpc::Status DecodeGeneralizedTime(std::span<const std::uint8_t> value,
                                   std::string* output) {
  if (output == nullptr || value.empty() || value.size() > kMaxIdentifierBytes) {
    return Invalid("MMS文件修改时间无效");
  }
  output->assign(reinterpret_cast<const char*>(value.data()), value.size());
  return grpc::Status::OK;
}

grpc::Status DecodeFileAttributes(std::span<const std::uint8_t> input,
                                  std::uint64_t* fileSize,
                                  bool* modifiedTimePresent,
                                  std::string* modifiedTime) {
  if (fileSize == nullptr || modifiedTimePresent == nullptr ||
      modifiedTime == nullptr) {
    return ArgumentError("MMS文件属性输出参数为空");
  }
  *fileSize = 0;
  *modifiedTimePresent = false;
  modifiedTime->clear();
  std::size_t offset = 0;
  BerTlvView size;
  auto status = ReadBerTlv(input, &offset, &size);
  if (!status.ok() || size.tag != 0x80) {
    return Invalid("MMS文件属性缺少文件大小");
  }
  std::uint64_t decodedSize = 0;
  status = ReadBerUnsigned(size.value, &decodedSize);
  if (!status.ok()) {
    return Invalid("MMS文件大小不是合法无符号整数");
  }
  *fileSize = decodedSize;
  if (offset < input.size()) {
    BerTlvView modified;
    status = ReadBerTlv(input, &offset, &modified);
    if (!status.ok() || modified.tag != 0x81) {
      return Invalid("MMS文件属性修改时间标签错误");
    }
    status = DecodeGeneralizedTime(modified.value, modifiedTime);
    if (!status.ok()) {
      return status;
    }
    *modifiedTimePresent = true;
  }
  if (offset != input.size()) {
    return Invalid("MMS文件属性包含多余字段");
  }
  return grpc::Status::OK;
}

grpc::Status EncodeFileAttributes(const MmsFileAttributes& attributes,
                                  std::uint64_t fallbackSize,
                                  std::string_view fallbackTime,
                                  std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return ArgumentError("MMS文件属性输出参数为空");
  }
  const auto size = attributes.sizePresent ? attributes.size : fallbackSize;
  std::vector<std::uint8_t> sizeValue;
  auto status = EncodeIntegerContent(false, 0, size, &sizeValue);
  if (!status.ok()) {
    return status;
  }
  output->clear();
  std::vector<std::uint8_t> sizeField;
  status = AppendTlv(0x80, sizeValue, &sizeField);
  if (!status.ok()) {
    return status;
  }
  output->insert(output->end(), sizeField.begin(), sizeField.end());
  const auto time = attributes.lastModifiedPresent ? attributes.lastModified
                                                   : fallbackTime;
  if (!time.empty()) {
    std::vector<std::uint8_t> timeField;
    status = AppendTlv(
        0x81,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(time.data()), time.size()),
        &timeField);
    if (!status.ok()) {
      return status;
    }
    output->insert(output->end(), timeField.begin(), timeField.end());
  }
  return grpc::Status::OK;
}

grpc::Status ReadTypeSignedWidth(std::span<const std::uint8_t> value,
                                 std::uint32_t* width) {
  if (width == nullptr) {
    return ArgumentError("TypeSpecification有符号位宽输出参数为空");
  }
  std::int64_t decoded = 0;
  auto status = ReadBerSigned(value, &decoded);
  if (!status.ok() || decoded < 0) {
    return Invalid("TypeSpecification位宽不是非负整数");
  }
  if (static_cast<std::uint64_t>(decoded) > kMaxTypeWidth) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "TypeSpecification位宽超过下位机上限");
  }
  *width = static_cast<std::uint32_t>(decoded);
  return grpc::Status::OK;
}

grpc::Status DecodeNamedType(std::span<const std::uint8_t> value,
                             std::string* typeName) {
  if (typeName == nullptr) {
    return ArgumentError("TypeSpecification命名类型输出参数为空");
  }
  std::size_t offset = 0;
  BerTlvView name;
  auto status = ReadBerTlv(value, &offset, &name);
  if (!status.ok() || offset != value.size()) {
    return Invalid("TypeSpecification命名类型对象名结构错误");
  }
  if (name.tag == 0x80 || name.tag == 0x82) {
    if (name.value.empty() || name.value.size() > kMaxIdentifierBytes) {
      return Invalid("TypeSpecification命名类型Identifier无效");
    }
    typeName->assign(reinterpret_cast<const char*>(name.value.data()),
                     name.value.size());
    return grpc::Status::OK;
  }
  if (name.tag != 0xa1) {
    return Invalid("TypeSpecification命名类型对象名选择错误");
  }
  std::size_t nestedOffset = 0;
  BerTlvView domain;
  BerTlvView item;
  status = ReadBerTlv(name.value, &nestedOffset, &domain);
  if (!status.ok() || domain.tag != 0x1a || domain.value.empty() ||
      domain.value.size() > kMaxIdentifierBytes) {
    return Invalid("TypeSpecification命名类型Domain标识错误");
  }
  status = ReadBerTlv(name.value, &nestedOffset, &item);
  if (!status.ok() || nestedOffset != name.value.size() || item.tag != 0x1a ||
      item.value.empty() || item.value.size() > kMaxIdentifierBytes) {
    return Invalid("TypeSpecification命名类型Item标识错误");
  }
  typeName->reserve(domain.value.size() + item.value.size() + 1);
  typeName->assign(reinterpret_cast<const char*>(domain.value.data()),
                   domain.value.size());
  typeName->push_back('$');
  typeName->append(reinterpret_cast<const char*>(item.value.data()),
                   item.value.size());
  return grpc::Status::OK;
}

grpc::Status DecodeTypeSpecification(std::span<const std::uint8_t> input,
                                     std::size_t depth,
                                     std::size_t* componentBudget,
                                     MmsTypeSpecification* result) {
  if (componentBudget == nullptr || result == nullptr) {
    return ArgumentError("TypeSpecification输出参数为空");
  }
  if (depth > kMaxTypeSpecificationDepth) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "TypeSpecification嵌套深度超过下位机上限");
  }
  std::size_t offset = 0;
  BerTlvView type;
  auto status = ReadBerTlv(input, &offset, &type);
  if (!status.ok() || offset != input.size()) {
    return Invalid("TypeSpecification必须包含单个完整选择");
  }
  *result = {};
  switch (type.tag) {
    case 0xa0:
      result->kind = MmsTypeSpecificationKind::NAMED_TYPE;
      return DecodeNamedType(type.value, &result->typeName);
    case 0xa1: {
      result->kind = MmsTypeSpecificationKind::ARRAY;
      std::size_t fieldOffset = 0;
      BerTlvView packed;
      BerTlvView count;
      BerTlvView element;
      status = ReadBerTlv(type.value, &fieldOffset, &packed);
      if (!status.ok() || packed.tag != 0x80) {
        return Invalid("TypeSpecification数组缺少packed字段");
      }
      bool packedValue = false;
      status = ReadBerBoolean(packed.value, &packedValue);
      if (!status.ok()) {
        return Invalid("TypeSpecification数组packed字段无效");
      }
      status = ReadBerTlv(type.value, &fieldOffset, &count);
      if (!status.ok() || count.tag != 0x81) {
        return Invalid("TypeSpecification数组缺少元素数量");
      }
      std::uint64_t elementCount = 0;
      status = ReadBerUnsigned(count.value, &elementCount);
      if (!status.ok() || elementCount > std::numeric_limits<std::uint32_t>::max()) {
        return Invalid("TypeSpecification数组元素数量无效");
      }
      result->elementCount = static_cast<std::uint32_t>(elementCount);
      status = ReadBerTlv(type.value, &fieldOffset, &element);
      if (!status.ok() || fieldOffset != type.value.size() || element.tag != 0xa2) {
        return Invalid("TypeSpecification数组元素类型结构错误");
      }
      result->elementType = std::make_shared<MmsTypeSpecification>();
      return DecodeTypeSpecification(element.value, depth + 1,
                                     componentBudget,
                                     result->elementType.get());
    }
    case 0xa2: {
      result->kind = MmsTypeSpecificationKind::STRUCTURE;
      std::size_t fieldOffset = 0;
      BerTlvView components;
      status = ReadBerTlv(type.value, &fieldOffset, &components);
      if (!status.ok() || fieldOffset != type.value.size() ||
          components.tag != 0xa1) {
        return Invalid("TypeSpecification结构缺少components字段");
      }
      std::size_t componentOffset = 0;
      while (componentOffset < components.value.size()) {
        if (*componentBudget == 0) {
          return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                              "TypeSpecification结构成员超过下位机上限");
        }
        --*componentBudget;
        BerTlvView component;
        status = ReadBerTlv(components.value, &componentOffset, &component);
        if (!status.ok() || component.tag != 0x30) {
          return Invalid("TypeSpecification结构成员不是SEQUENCE");
        }
        std::size_t memberOffset = 0;
        BerTlvView name;
        BerTlvView memberType;
        status = ReadBerTlv(component.value, &memberOffset, &name);
        if (!status.ok()) {
          return Invalid("TypeSpecification结构成员字段缺失");
        }
        const bool hasName = name.tag == 0x80;
        if (hasName && (name.value.empty() ||
                        name.value.size() > kMaxIdentifierBytes)) {
          return Invalid("TypeSpecification结构成员名称无效");
        }
        if (!hasName) {
          memberType = name;
        } else {
          status = ReadBerTlv(component.value, &memberOffset, &memberType);
        }
        if (!status.ok() || memberOffset != component.value.size() ||
            memberType.tag != 0xa1) {
          return Invalid("TypeSpecification结构成员类型结构错误");
        }
        auto& decoded = result->components.emplace_back();
        if (hasName) {
          decoded.name.assign(
              reinterpret_cast<const char*>(name.value.data()), name.value.size());
        }
        decoded.type = std::make_shared<MmsTypeSpecification>();
        status = DecodeTypeSpecification(memberType.value, depth + 1,
                                         componentBudget, decoded.type.get());
        if (!status.ok() && !memberType.value.empty()) {
          // componentType使用隐式上下文标签时，数组/结构的选择内容直接
          // 位于a1/a2字段内，不再重复携带同一个选择标签。仅对这两种
          // 可由首字段明确识别的形状补回包装，避免吞掉未知尾部字段。
          const bool implicitArray =
              memberType.tag == 0xa1 && memberType.value.front() == 0x80;
          const bool implicitStructure =
              memberType.tag == 0xa2 && memberType.value.front() == 0x30;
          if (implicitArray || implicitStructure) {
            std::vector<std::uint8_t> wrapped;
            status = AppendTlv(memberType.tag, memberType.value, &wrapped);
            if (status.ok()) {
              status = DecodeTypeSpecification(wrapped, depth + 1,
                                               componentBudget,
                                               decoded.type.get());
            }
          }
        }
        if (!status.ok()) {
          return status;
        }
      }
      return grpc::Status::OK;
    }
    case 0x83:
      if (!type.value.empty()) {
        return Invalid("TypeSpecification BOOLEAN必须为空值");
      }
      result->kind = MmsTypeSpecificationKind::BOOLEAN;
      return grpc::Status::OK;
    case 0x84:
      result->kind = MmsTypeSpecificationKind::BIT_STRING;
      return ReadTypeSignedWidth(type.value, &result->width);
    case 0x85:
      result->kind = MmsTypeSpecificationKind::INTEGER;
      return ReadTypeSignedWidth(type.value, &result->width);
    case 0x86:
      result->kind = MmsTypeSpecificationKind::UNSIGNED;
      return ReadTypeSignedWidth(type.value, &result->width);
    case 0xa7: {
      result->kind = MmsTypeSpecificationKind::FLOATING_POINT;
      std::size_t fieldOffset = 0;
      BerTlvView formatWidth;
      BerTlvView exponentWidth;
      status = ReadBerTlv(type.value, &fieldOffset, &formatWidth);
      if (!status.ok() || formatWidth.tag != 0x02) {
        return Invalid("TypeSpecification浮点格式宽度无效");
      }
      status = ReadTypeSignedWidth(formatWidth.value, &result->width);
      if (!status.ok()) {
        return status;
      }
      status = ReadBerTlv(type.value, &fieldOffset, &exponentWidth);
      if (!status.ok() || fieldOffset != type.value.size() ||
          exponentWidth.tag != 0x02) {
        return Invalid("TypeSpecification浮点指数宽度无效");
      }
      return ReadTypeSignedWidth(exponentWidth.value, &result->exponentWidth);
    }
    case 0x89:
      result->kind = MmsTypeSpecificationKind::OCTET_STRING;
      return ReadTypeSignedWidth(type.value, &result->width);
    case 0x8a:
      result->kind = MmsTypeSpecificationKind::VISIBLE_STRING;
      return ReadTypeSignedWidth(type.value, &result->width);
    case 0x8b:
      if (!type.value.empty()) {
        return Invalid("TypeSpecification GeneralTime必须为空值");
      }
      result->kind = MmsTypeSpecificationKind::GENERAL_TIME;
      return grpc::Status::OK;
    case 0x8c:
      if (type.value.size() != 1 || type.value.front() > 1) {
        return Invalid("TypeSpecification BinaryTime宽度无效");
      }
      result->kind = MmsTypeSpecificationKind::BINARY_TIME;
      result->binaryTimeWidth = type.value.front() == 0 ? 4 : 6;
      return grpc::Status::OK;
    case 0x8d:
      result->kind = MmsTypeSpecificationKind::BCD;
      return ReadTypeSignedWidth(type.value, &result->width);
    case 0x8e:
      if (!type.value.empty()) {
        return Invalid("TypeSpecification ObjectIdentifier必须为空值");
      }
      result->kind = MmsTypeSpecificationKind::OBJECT_IDENTIFIER;
      return grpc::Status::OK;
    case 0x8f:
      result->kind = MmsTypeSpecificationKind::MMS_STRING;
      return ReadTypeSignedWidth(type.value, &result->width);
    case 0x90:
      result->kind = MmsTypeSpecificationKind::UTF8_STRING;
      return ReadTypeSignedWidth(type.value, &result->width);
    case 0x91:
      if (!type.value.empty()) {
        return Invalid("TypeSpecification UtcTime必须为空值");
      }
      result->kind = MmsTypeSpecificationKind::UTC_TIME;
      return grpc::Status::OK;
    default:
      return Invalid("TypeSpecification包含不支持的选择");
  }
}

}  // namespace

grpc::Status DecodeMmsObjectName(std::span<const std::uint8_t> input,
                                 MmsObjectName* name) {
  return DecodeObjectName(input, name);
}

grpc::Status EncodeMmsObjectName(const MmsObjectName& name,
                                 std::vector<std::uint8_t>* encoded) {
  return EncodeObjectName(name, encoded);
}

grpc::Status EncodeMmsConfirmedRequest(
    std::uint32_t invokeId, std::uint32_t serviceTag,
    std::span<const std::uint8_t> serviceValue,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeConfirmed(0xa0, invokeId, serviceTag, serviceValue, true,
                         output, outputSize);
}

grpc::Status EncodeMmsConfirmedPrimitiveRequest(
    std::uint32_t invokeId, std::uint32_t serviceTag,
    std::span<const std::uint8_t> serviceValue,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeConfirmed(0xa0, invokeId, serviceTag, serviceValue, false,
                         output, outputSize);
}

grpc::Status DecodeMmsConfirmedRequest(std::span<const std::uint8_t> input,
                                        MmsConfirmedPduView* pdu) {
  return DecodeConfirmed(input, 0xa0, pdu);
}

grpc::Status EncodeMmsConfirmedResponse(
    std::uint32_t invokeId, std::uint32_t serviceTag,
    std::span<const std::uint8_t> serviceValue,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeConfirmed(0xa1, invokeId, serviceTag, serviceValue, true,
                         output, outputSize);
}

grpc::Status DecodeMmsConfirmedResponse(std::span<const std::uint8_t> input,
                                         MmsConfirmedPduView* pdu) {
  return DecodeConfirmed(input, 0xa1, pdu);
}

grpc::Status DecodeMmsConfirmedError(
    std::span<const std::uint8_t> input, MmsConfirmedErrorPduView* pdu) {
  if (pdu == nullptr) {
    return ArgumentError("Confirmed-ErrorPDU输出参数为空");
  }
  *pdu = {};
  MmsConfirmedErrorPduView decoded;
  std::size_t offset = 0;
  BerTlvView outer;
  auto status = ReadBerTlv(input, &offset, &outer);
  if (!status.ok() || offset != input.size() || outer.tag != 0xa2) {
    return Invalid("Confirmed-ErrorPDU外层标签或长度错误");
  }

  std::size_t contentOffset = 0;
  BerTlvView invoke;
  status = ReadBerTlv(outer.value, &contentOffset, &invoke);
  if (!status.ok() || invoke.tag != 0x02) {
    return Invalid("Confirmed-ErrorPDU缺少invokeID");
  }
  std::uint64_t invokeId = 0;
  status = ReadBerUnsigned(invoke.value, &invokeId);
  if (!status.ok() || invokeId == 0 ||
      invokeId > std::numeric_limits<std::uint32_t>::max()) {
    return Invalid("Confirmed-ErrorPDU invokeID超出范围");
  }

  BerTlvView serviceError;
  status = ReadBerTlv(outer.value, &contentOffset, &serviceError);
  if (!status.ok() || contentOffset != outer.value.size() ||
      serviceError.tag != 0x30) {
    return Invalid("Confirmed-ErrorPDU缺少ServiceError");
  }

  std::size_t errorOffset = 0;
  BerTlvView errorClass;
  status = ReadBerTlv(serviceError.value, &errorOffset, &errorClass);
  if (!status.ok() || errorClass.tag < 0x80 || errorClass.tag > 0x8b) {
    return Invalid("Confirmed-ErrorPDU错误类选择无效");
  }
  std::int64_t errorCode = 0;
  status = ReadBerSigned(errorClass.value, &errorCode);
  if (!status.ok() || errorCode < std::numeric_limits<std::int32_t>::min() ||
      errorCode > std::numeric_limits<std::int32_t>::max()) {
    return Invalid("Confirmed-ErrorPDU错误码超出Integer32范围");
  }

  bool seenAdditionalCode = false;
  bool seenAdditionalDescription = false;
  bool seenServiceSpecificInformation = false;
  bool seenModifierPosition = false;
  while (errorOffset < serviceError.value.size()) {
    BerTlvView field;
    status = ReadBerTlv(serviceError.value, &errorOffset, &field);
    if (!status.ok()) {
      return status;
    }
    switch (field.tag) {
      case 0x81: {
        if (seenAdditionalCode) {
          return Invalid("Confirmed-ErrorPDU重复additionalCode");
        }
        std::int64_t value = 0;
        status = ReadBerSigned(field.value, &value);
        if (!status.ok() ||
            value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) {
          return Invalid("Confirmed-ErrorPDU additionalCode超出Integer32范围");
        }
        decoded.additionalCode = value;
        seenAdditionalCode = true;
        break;
      }
      case 0x82:
        if (seenAdditionalDescription || field.value.size() > kMaxIdentifierBytes) {
          return Invalid("Confirmed-ErrorPDU additionalDescription无效");
        }
        decoded.additionalDescription.assign(
            reinterpret_cast<const char*>(field.value.data()),
            field.value.size());
        seenAdditionalDescription = true;
        break;
      case 0x83:
        if (seenServiceSpecificInformation) {
          return Invalid("Confirmed-ErrorPDU重复serviceSpecificInformation");
        }
        decoded.serviceSpecificInformation = field.value;
        seenServiceSpecificInformation = true;
        break;
      case 0x84: {
        if (seenModifierPosition) {
          return Invalid("Confirmed-ErrorPDU重复modifierPosition");
        }
        std::int64_t value = 0;
        status = ReadBerSigned(field.value, &value);
        if (!status.ok() ||
            value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) {
          return Invalid("Confirmed-ErrorPDU modifierPosition超出Integer32范围");
        }
        decoded.modifierPosition = value;
        seenModifierPosition = true;
        break;
      }
      default:
        return Invalid("Confirmed-ErrorPDU包含未知ServiceError字段");
    }
  }

  decoded.invokeId = static_cast<std::uint32_t>(invokeId);
  decoded.errorClass = static_cast<std::uint8_t>(errorClass.tag & 0x1f);
  decoded.errorCode = errorCode;
  *pdu = std::move(decoded);
  return grpc::Status::OK;
}

grpc::Status EncodeMmsIdentifyRequest(std::uint32_t invokeId,
                                      std::span<std::uint8_t> output,
                                      std::size_t* outputSize) {
  return EncodeMmsConfirmedRequest(invokeId, 3, {}, output, outputSize);
}

grpc::Status DecodeMmsIdentifyResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsIdentifyResponse* response) {
  if (response == nullptr) {
    return ArgumentError("Identify响应输出参数为空");
  }
  *response = {};
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedResponse(input, &pdu);
  if (!status.ok()) {
    return status;
  }
  if (pdu.invokeId != expectedInvokeId || pdu.serviceTag != 3) {
    return Invalid("Identify响应invokeID或服务选择不匹配");
  }
  std::size_t offset = 0;
  BerTlvView sequence;
  status = ReadBerTlv(pdu.serviceValue, &offset, &sequence);
  if (!status.ok() || offset != pdu.serviceValue.size() ||
      sequence.tag != 0x30) {
    return Invalid("Identify响应缺少厂商信息序列");
  }
  std::size_t fieldOffset = 0;
  const auto readField = [&sequence, &fieldOffset](std::uint8_t tag,
                                                   std::string* value) {
    if (value == nullptr) {
      return false;
    }
    BerTlvView field;
    if (!ReadBerTlv(sequence.value, &fieldOffset, &field).ok() ||
        field.tag != tag ||
        field.value.empty() || field.value.size() > kMaxIdentifierBytes) {
      return false;
    }
    value->assign(reinterpret_cast<const char*>(field.value.data()),
                  field.value.size());
    return true;
  };
  if (!readField(0x80, &response->vendorName) ||
      !readField(0x81, &response->modelName) ||
      !readField(0x82, &response->revision) ||
      fieldOffset != sequence.value.size()) {
    *response = {};
    return Invalid("Identify响应字段缺失或长度无效");
  }
  return grpc::Status::OK;
}

grpc::Status EncodeMmsFileDirectoryRequest(
    std::uint32_t invokeId, const MmsFileDirectoryRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  std::vector<std::uint8_t> service;
  if (!request.fileSpecification.empty()) {
    std::vector<std::uint8_t> fileName;
    auto status = AppendFileNameList(request.fileSpecification, &fileName);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> field;
    status = AppendTlv(0xa0, fileName, &field);
    if (!status.ok()) {
      return status;
    }
    service.insert(service.end(), field.begin(), field.end());
  }
  if (request.continueAfter.has_value()) {
    if (request.continueAfter->empty()) {
      return ArgumentError("MMS文件目录continueAfter不能为空");
    }
    std::vector<std::uint8_t> fileName;
    auto status = AppendFileNameList(*request.continueAfter, &fileName);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> field;
    status = AppendTlv(0xa1, fileName, &field);
    if (!status.ok()) {
      return status;
    }
    service.insert(service.end(), field.begin(), field.end());
  }
  return EncodeMmsConfirmedRequest(invokeId, 77, service, output, outputSize);
}

grpc::Status DecodeMmsFileDirectoryResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsFileDirectoryResponse* response) {
  if (response == nullptr) {
    return ArgumentError("MMS文件目录响应输出参数为空");
  }
  *response = {};
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedResponse(input, &pdu);
  if (!status.ok()) {
    return status;
  }
  if (pdu.invokeId != expectedInvokeId || pdu.serviceTag != 77) {
    return Invalid("MMS文件目录响应invokeID或服务选择不匹配");
  }
  std::size_t offset = 0;
  BerTlvView list;
  status = ReadBerTlv(pdu.serviceValue, &offset, &list);
  if (!status.ok() || list.tag != 0xa0) {
    return Invalid("MMS文件目录响应缺少目录列表");
  }
  std::size_t listOffset = 0;
  BerTlvView sequence;
  status = ReadBerTlv(list.value, &listOffset, &sequence);
  if (!status.ok() || listOffset != list.value.size() || sequence.tag != 0x30) {
    return Invalid("MMS文件目录响应目录列表不是SEQUENCE");
  }
  std::size_t entriesOffset = 0;
  while (entriesOffset < sequence.value.size()) {
    if (response->entries.size() >= kMaxNameListEntries) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "MMS文件目录条目超过下位机上限");
    }
    BerTlvView entry;
    status = ReadBerTlv(sequence.value, &entriesOffset, &entry);
    if (!status.ok() || entry.tag != 0x30) {
      return Invalid("MMS文件目录项不是SEQUENCE");
    }
    std::size_t entryOffset = 0;
    BerTlvView fileName;
    BerTlvView attributes;
    status = ReadBerTlv(entry.value, &entryOffset, &fileName);
    if (!status.ok() || fileName.tag != 0xa0) {
      return Invalid("MMS文件目录项缺少文件名");
    }
    status = ReadBerTlv(entry.value, &entryOffset, &attributes);
    if (!status.ok() || entryOffset != entry.value.size() ||
        attributes.tag != 0xa1) {
      return Invalid("MMS文件目录项缺少文件属性");
    }
    MmsFileDirectoryEntry decoded;
    status = DecodeFileNameList(fileName.value, &decoded.fileName);
    if (!status.ok()) {
      return status;
    }
    status = DecodeFileAttributes(attributes.value, &decoded.fileSize,
                                   &decoded.modifiedTimePresent,
                                   &decoded.modifiedTime);
    if (!status.ok()) {
      return status;
    }
    decoded.attributes.sizePresent = true;
    decoded.attributes.size = decoded.fileSize;
    decoded.attributes.lastModifiedPresent = decoded.modifiedTimePresent;
    decoded.attributes.lastModified = decoded.modifiedTime;
    response->entries.emplace_back(std::move(decoded));
  }
  if (offset < pdu.serviceValue.size()) {
    BerTlvView more;
    status = ReadBerTlv(pdu.serviceValue, &offset, &more);
    if (!status.ok() || more.tag != 0x81) {
      return Invalid("MMS文件目录响应moreFollows字段错误");
    }
    status = ReadBerBoolean(more.value, &response->moreFollows);
    if (!status.ok()) {
      return Invalid("MMS文件目录响应moreFollows值错误");
    }
  }
  if (offset != pdu.serviceValue.size()) {
    return Invalid("MMS文件目录响应包含多余字段");
  }
  return grpc::Status::OK;
}

grpc::Status EncodeMmsFileDirectoryResponse(
    std::uint32_t invokeId, const MmsFileDirectoryResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (response.entries.size() > kMaxNameListEntries) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "MMS文件目录条目超过下位机上限");
  }
  std::vector<std::uint8_t> entries;
  for (auto entryIt = response.entries.rbegin();
       entryIt != response.entries.rend(); ++entryIt) {
    if (entryIt->fileName.empty()) {
      return ArgumentError("MMS文件目录项文件名为空");
    }
    std::vector<std::uint8_t> name;
    auto status = AppendFileNameList(entryIt->fileName, &name);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> nameField;
    status = AppendTlv(0xa0, name, &nameField);
    if (!status.ok()) {
      return status;
    }
    MmsFileAttributes attributes = entryIt->attributes;
    if (!attributes.sizePresent && entryIt->fileSize != 0) {
      attributes.sizePresent = true;
      attributes.size = entryIt->fileSize;
    }
    if (!attributes.lastModifiedPresent && !entryIt->modifiedTime.empty()) {
      attributes.lastModifiedPresent = entryIt->modifiedTimePresent;
      attributes.lastModified = entryIt->modifiedTime;
    }
    std::vector<std::uint8_t> attrValue;
    status = EncodeFileAttributes(attributes, entryIt->fileSize,
                                  entryIt->modifiedTime, &attrValue);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> attrField;
    status = AppendTlv(0xa1, attrValue, &attrField);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> entryValue;
    entryValue.reserve(nameField.size() + attrField.size());
    entryValue.insert(entryValue.end(), nameField.begin(), nameField.end());
    entryValue.insert(entryValue.end(), attrField.begin(), attrField.end());
    std::vector<std::uint8_t> encodedEntry;
    status = AppendTlv(0x30, entryValue, &encodedEntry);
    if (!status.ok()) {
      return status;
    }
    entries.insert(entries.begin(), encodedEntry.begin(), encodedEntry.end());
  }
  std::vector<std::uint8_t> sequence;
  auto status = AppendTlv(0x30, entries, &sequence);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> list;
  status = AppendTlv(0xa0, sequence, &list);
  if (!status.ok()) {
    return status;
  }
  if (response.moreFollows) {
    std::vector<std::uint8_t> more;
    status = AppendTlv(0x81, std::array<std::uint8_t, 1>{0xff}, &more);
    if (!status.ok()) {
      return status;
    }
    list.insert(list.end(), more.begin(), more.end());
  }
  return EncodeMmsConfirmedResponse(invokeId, 77, list, output, outputSize);
}

grpc::Status DecodeMmsFileDirectoryRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsFileDirectoryRequest* request) {
  if (invokeId == nullptr || request == nullptr) {
    return ArgumentError("MMS文件目录请求输出参数为空");
  }
  *invokeId = 0;
  *request = {};
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedRequest(input, &pdu);
  if (!status.ok() || pdu.serviceTag != 77) {
    return Invalid("MMS文件目录请求服务选择错误");
  }
  std::size_t offset = 0;
  while (offset < pdu.serviceValue.size()) {
    BerTlvView field;
    status = ReadBerTlv(pdu.serviceValue, &offset, &field);
    if (!status.ok() || (field.tag != 0xa0 && field.tag != 0xa1)) {
      return Invalid("MMS文件目录请求字段错误");
    }
    std::string value;
    status = DecodeFileNameList(field.value, &value);
    if (!status.ok()) {
      return status;
    }
    if (field.tag == 0xa0) {
      if (!request->fileSpecification.empty()) {
        return Invalid("MMS文件目录请求重复fileSpecification");
      }
      request->fileSpecification = std::move(value);
    } else {
      if (request->continueAfter.has_value()) {
        return Invalid("MMS文件目录请求重复continueAfter");
      }
      request->continueAfter = std::move(value);
    }
  }
  *invokeId = pdu.invokeId;
  return grpc::Status::OK;
}

grpc::Status EncodeMmsFileOpenRequest(
    std::uint32_t invokeId, std::string_view fileName,
    std::uint32_t initialPosition, std::span<std::uint8_t> output,
    std::size_t* outputSize) {
  std::vector<std::uint8_t> name;
  auto status = AppendFileNameList(fileName, &name);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> service;
  status = AppendTlv(0xa0, name, &service);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> position;
  status = EncodeIntegerContent(false, 0, initialPosition, &position);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> positionField;
  status = AppendTlv(0x81, position, &positionField);
  if (!status.ok()) {
    return status;
  }
  service.insert(service.end(), positionField.begin(), positionField.end());
  return EncodeMmsConfirmedRequest(invokeId, 72, service, output, outputSize);
}

grpc::Status EncodeMmsFileOpenRequest(
    std::uint32_t invokeId, const MmsFileOpenRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeMmsFileOpenRequest(invokeId, request.fileName,
                                  request.initialPosition, output, outputSize);
}

grpc::Status DecodeMmsFileOpenRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsFileOpenRequest* request) {
  if (invokeId == nullptr || request == nullptr) {
    return ArgumentError("MMS文件打开请求输出参数为空");
  }
  *invokeId = 0;
  *request = {};
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedRequest(input, &pdu);
  if (!status.ok() || pdu.serviceTag != 72) {
    return Invalid("MMS文件打开请求服务选择错误");
  }
  std::size_t offset = 0;
  BerTlvView name;
  status = ReadBerTlv(pdu.serviceValue, &offset, &name);
  if (!status.ok() || name.tag != 0xa0) {
    return Invalid("MMS文件打开请求缺少文件名");
  }
  status = DecodeFileNameList(name.value, &request->fileName);
  if (!status.ok()) {
    return status;
  }
  BerTlvView position;
  status = ReadBerTlv(pdu.serviceValue, &offset, &position);
  if (!status.ok() || offset != pdu.serviceValue.size() ||
      position.tag != 0x81) {
    return Invalid("MMS文件打开请求缺少初始位置");
  }
  std::uint64_t decodedPosition = 0;
  status = ReadBerUnsigned(position.value, &decodedPosition);
  if (!status.ok() || decodedPosition > std::numeric_limits<std::uint32_t>::max()) {
    return Invalid("MMS文件打开请求初始位置无效");
  }
  request->initialPosition = static_cast<std::uint32_t>(decodedPosition);
  *invokeId = pdu.invokeId;
  return grpc::Status::OK;
}

grpc::Status DecodeMmsFileOpenResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsFileOpenResponse* response) {
  if (response == nullptr) {
    return ArgumentError("MMS文件打开响应输出参数为空");
  }
  *response = {};
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedResponse(input, &pdu);
  if (!status.ok()) {
    return status;
  }
  if (pdu.invokeId != expectedInvokeId || pdu.serviceTag != 72) {
    return Invalid("MMS文件打开响应invokeID或服务选择不匹配");
  }
  std::size_t offset = 0;
  BerTlvView frsm;
  BerTlvView attributes;
  status = ReadBerTlv(pdu.serviceValue, &offset, &frsm);
  if (!status.ok() || frsm.tag != 0x80) {
    return Invalid("MMS文件打开响应缺少FRSMID");
  }
  std::int64_t frsmId = 0;
  status = ReadBerSigned(frsm.value, &frsmId);
  if (!status.ok() || frsmId < std::numeric_limits<std::int32_t>::min() ||
      frsmId > std::numeric_limits<std::int32_t>::max()) {
    return Invalid("MMS文件打开响应FRSMID无效");
  }
  status = ReadBerTlv(pdu.serviceValue, &offset, &attributes);
  if (!status.ok() || offset != pdu.serviceValue.size() ||
      attributes.tag != 0xa1) {
    return Invalid("MMS文件打开响应缺少文件属性");
  }
  response->frsmId = static_cast<std::int32_t>(frsmId);
  status = DecodeFileAttributes(attributes.value, &response->fileSize,
                                &response->modifiedTimePresent,
                                &response->modifiedTime);
  if (status.ok()) {
    response->attributes.sizePresent = true;
    response->attributes.size = response->fileSize;
    response->attributes.lastModifiedPresent = response->modifiedTimePresent;
    response->attributes.lastModified = response->modifiedTime;
  }
  return status;
}

grpc::Status EncodeMmsFileOpenResponse(
    std::uint32_t invokeId, const MmsFileOpenResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  std::vector<std::uint8_t> idValue;
  auto status = EncodeIntegerContent(true, response.frsmId, 0, &idValue);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> id;
  status = AppendTlv(0x80, idValue, &id);
  if (!status.ok()) {
    return status;
  }
  MmsFileAttributes attributes = response.attributes;
  if (!attributes.sizePresent) {
    attributes.sizePresent = true;
    attributes.size = response.fileSize;
  }
  if (!attributes.lastModifiedPresent && response.modifiedTimePresent) {
    attributes.lastModifiedPresent = true;
    attributes.lastModified = response.modifiedTime;
  }
  std::vector<std::uint8_t> attrValue;
  status = EncodeFileAttributes(attributes, response.fileSize,
                                response.modifiedTime, &attrValue);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> attr;
  status = AppendTlv(0xa1, attrValue, &attr);
  if (!status.ok()) {
    return status;
  }
  id.insert(id.end(), attr.begin(), attr.end());
  return EncodeMmsConfirmedResponse(invokeId, 72, id, output, outputSize);
}

grpc::Status EncodeMmsFileReadRequest(
    std::uint32_t invokeId, std::int32_t frsmId,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  std::vector<std::uint8_t> value;
  auto status = EncodeIntegerContent(true, frsmId, 0, &value);
  if (!status.ok()) {
    return status;
  }
  return EncodeMmsConfirmedPrimitiveRequest(invokeId, 73, value, output,
                                            outputSize);
}

grpc::Status EncodeMmsFileReadRequest(
    std::uint32_t invokeId, const MmsFileReadRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeMmsFileReadRequest(invokeId, request.frsmId, output, outputSize);
}

grpc::Status DecodeMmsFileReadRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsFileReadRequest* request) {
  if (invokeId == nullptr || request == nullptr) {
    return ArgumentError("MMS文件读取请求输出参数为空");
  }
  *invokeId = 0;
  *request = {};
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedRequest(input, &pdu);
  if (!status.ok() || pdu.serviceTag != 73) {
    return Invalid("MMS文件读取请求服务选择错误");
  }
  std::int64_t decoded = 0;
  status = ReadBerSigned(pdu.serviceValue, &decoded);
  if (!status.ok() || decoded < std::numeric_limits<std::int32_t>::min() ||
      decoded > std::numeric_limits<std::int32_t>::max()) {
    return Invalid("MMS文件读取请求FRSMID无效");
  }
  request->frsmId = static_cast<std::int32_t>(decoded);
  *invokeId = pdu.invokeId;
  return grpc::Status::OK;
}

grpc::Status DecodeMmsFileReadResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsFileReadResponse* response) {
  if (response == nullptr) {
    return ArgumentError("MMS文件读取响应输出参数为空");
  }
  *response = {};
  response->moreFollows = true;
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedResponse(input, &pdu);
  if (!status.ok()) {
    return status;
  }
  if (pdu.invokeId != expectedInvokeId || pdu.serviceTag != 73) {
    return Invalid("MMS文件读取响应invokeID或服务选择不匹配");
  }
  std::size_t offset = 0;
  BerTlvView data;
  status = ReadBerTlv(pdu.serviceValue, &offset, &data);
  if (!status.ok() || data.tag != 0x80) {
    return Invalid("MMS文件读取响应缺少文件数据");
  }
  if (data.value.size() > kMaxMmsPduBytes) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "MMS文件分段超过下位机上限");
  }
  response->data.assign(data.value.begin(), data.value.end());
  response->fileData = response->data;
  if (offset < pdu.serviceValue.size()) {
    BerTlvView more;
    status = ReadBerTlv(pdu.serviceValue, &offset, &more);
    if (!status.ok() || more.tag != 0x81) {
      return Invalid("MMS文件读取响应moreFollows字段错误");
    }
    status = ReadBerBoolean(more.value, &response->moreFollows);
    if (!status.ok()) {
      return Invalid("MMS文件读取响应moreFollows值错误");
    }
  }
  if (offset != pdu.serviceValue.size()) {
    return Invalid("MMS文件读取响应包含多余字段");
  }
  return grpc::Status::OK;
}

grpc::Status EncodeMmsFileReadResponse(
    std::uint32_t invokeId, const MmsFileReadResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  const auto& data = response.fileData.empty() ? response.data : response.fileData;
  if (data.size() > kMaxMmsPduBytes) {
    return OutputError("MMS文件分段超过下位机上限");
  }
  std::vector<std::uint8_t> service;
  auto status = AppendTlv(0x80, data, &service);
  if (!status.ok()) {
    return status;
  }
  if (!response.moreFollows) {
    std::vector<std::uint8_t> more;
    status = AppendTlv(0x81, std::array<std::uint8_t, 1>{0x00}, &more);
    if (!status.ok()) {
      return status;
    }
    service.insert(service.end(), more.begin(), more.end());
  }
  return EncodeMmsConfirmedResponse(invokeId, 73, service, output, outputSize);
}

grpc::Status EncodeMmsFileCloseRequest(
    std::uint32_t invokeId, std::int32_t frsmId,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  std::vector<std::uint8_t> value;
  auto status = EncodeIntegerContent(true, frsmId, 0, &value);
  if (!status.ok()) {
    return status;
  }
  return EncodeMmsConfirmedPrimitiveRequest(invokeId, 74, value, output,
                                            outputSize);
}

grpc::Status EncodeMmsFileCloseRequest(
    std::uint32_t invokeId, const MmsFileCloseRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeMmsFileCloseRequest(invokeId, request.frsmId, output, outputSize);
}

grpc::Status DecodeMmsFileCloseRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsFileCloseRequest* request) {
  if (invokeId == nullptr || request == nullptr) {
    return ArgumentError("MMS文件关闭请求输出参数为空");
  }
  *invokeId = 0;
  *request = {};
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedRequest(input, &pdu);
  if (!status.ok() || pdu.serviceTag != 74) {
    return Invalid("MMS文件关闭请求服务选择错误");
  }
  std::int64_t decoded = 0;
  status = ReadBerSigned(pdu.serviceValue, &decoded);
  if (!status.ok() || decoded < std::numeric_limits<std::int32_t>::min() ||
      decoded > std::numeric_limits<std::int32_t>::max()) {
    return Invalid("MMS文件关闭请求FRSMID无效");
  }
  request->frsmId = static_cast<std::int32_t>(decoded);
  *invokeId = pdu.invokeId;
  return grpc::Status::OK;
}

grpc::Status EncodeMmsFileCloseResponse(
    std::uint32_t invokeId, std::span<std::uint8_t> output,
    std::size_t* outputSize) {
  return EncodeMmsConfirmedResponse(invokeId, 74, {}, output, outputSize);
}

grpc::Status DecodeMmsFileCloseResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId) {
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedResponse(input, &pdu);
  if (!status.ok() || pdu.invokeId != expectedInvokeId ||
      pdu.serviceTag != 74 || !pdu.serviceValue.empty()) {
    return Invalid("MMS文件关闭响应invokeID、服务选择或内容错误");
  }
  return grpc::Status::OK;
}

grpc::Status EncodeMmsGetNameListRequest(
    std::uint32_t invokeId, const MmsGetNameListRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return ArgumentError("GetNameList请求输出长度参数为空");
  }
  *outputSize = 0;
  if (!IsSupportedObjectClass(
          static_cast<std::int64_t>(request.objectClass))) {
    return ArgumentError("GetNameList ObjectClass不受支持");
  }
  if (request.scope.type == MmsObjectScopeType::DOMAIN_SPECIFIC &&
      request.objectClass == MmsObjectClass::DOMAIN) {
    return ArgumentError("GetNameList Domain对象类不能使用Domain范围");
  }
  std::vector<std::uint8_t> classValue;
  auto status = AppendSigned(
      0x80, static_cast<std::int64_t>(request.objectClass), &classValue);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> objectClass;
  status = AppendTlv(0xa0, classValue, &objectClass);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> scope;
  status = EncodeObjectScope(request.scope, &scope);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> service;
  service.reserve(objectClass.size() + scope.size() + 16);
  service.insert(service.end(), objectClass.begin(), objectClass.end());
  std::vector<std::uint8_t> scopeField;
  status = AppendTlv(0xa1, scope, &scopeField);
  if (!status.ok()) {
    return status;
  }
  service.insert(service.end(), scopeField.begin(), scopeField.end());
  if (request.continueAfter.has_value()) {
    std::vector<std::uint8_t> continuation;
    status = AppendContextIdentifier(0x82, *request.continueAfter,
                                     &continuation);
    if (!status.ok()) {
      return status;
    }
    service.insert(service.end(), continuation.begin(), continuation.end());
  }
  return EncodeMmsConfirmedRequest(invokeId, 1, service, output, outputSize);
}

grpc::Status DecodeMmsGetNameListRequest(
    std::span<const std::uint8_t> input, std::uint32_t* invokeId,
    MmsGetNameListRequest* request) {
  if (invokeId == nullptr || request == nullptr) {
    return ArgumentError("GetNameList请求输出参数为空");
  }
  *invokeId = 0;
  *request = {};
  std::uint32_t decodedInvokeId = 0;
  MmsGetNameListRequest decoded;
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedRequest(input, &pdu);
  if (!status.ok()) {
    return status;
  }
  if (pdu.serviceTag != 1) {
    return Invalid("Confirmed请求不是GetNameList");
  }
  decodedInvokeId = pdu.invokeId;
  std::size_t offset = 0;
  BerTlvView objectClass;
  status = ReadBerTlv(pdu.serviceValue, &offset, &objectClass);
  if (!status.ok() || !IsContextConstructed(objectClass.tag, 0)) {
    return Invalid("GetNameList缺少ObjectClass");
  }
  std::size_t classOffset = 0;
  BerTlvView classInteger;
  status = ReadBerTlv(objectClass.value, &classOffset, &classInteger);
  if (!status.ok() || classOffset != objectClass.value.size() ||
      !IsContextPrimitive(classInteger.tag, 0)) {
    return Invalid("GetNameList ObjectClass结构错误");
  }
  std::int64_t classValue = 0;
  status = ReadBerSigned(classInteger.value, &classValue);
  if (!status.ok() || !IsSupportedObjectClass(classValue)) {
    return Invalid("GetNameList ObjectClass不受支持");
  }
  decoded.objectClass = static_cast<MmsObjectClass>(classValue);

  BerTlvView scope;
  status = ReadBerTlv(pdu.serviceValue, &offset, &scope);
  if (!status.ok() || !IsContextConstructed(scope.tag, 1)) {
    return Invalid("GetNameList缺少ObjectScope");
  }
  status = DecodeObjectScope(scope.value, &decoded.scope);
  if (!status.ok()) {
    return status;
  }
  if (decoded.scope.type == MmsObjectScopeType::DOMAIN_SPECIFIC &&
      decoded.objectClass == MmsObjectClass::DOMAIN) {
    return Invalid("GetNameList Domain对象类不能使用Domain范围");
  }
  if (offset < pdu.serviceValue.size()) {
    BerTlvView continuation;
    status = ReadBerTlv(pdu.serviceValue, &offset, &continuation);
    if (!status.ok() || offset != pdu.serviceValue.size() ||
        !IsContextPrimitive(continuation.tag, 2) ||
        continuation.value.empty() ||
        continuation.value.size() > kMaxIdentifierBytes) {
      return Invalid("GetNameList continueAfter错误");
    }
    decoded.continueAfter = std::string(
        reinterpret_cast<const char*>(continuation.value.data()),
        continuation.value.size());
  }
  if (offset != pdu.serviceValue.size()) {
    return Invalid("GetNameList包含未知字段");
  }
  *invokeId = decodedInvokeId;
  *request = std::move(decoded);
  return grpc::Status::OK;
}

grpc::Status DecodeMmsGetNameListResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsGetNameListResponse* response) {
  if (response == nullptr) {
    return ArgumentError("GetNameList响应输出参数为空");
  }
  *response = {};
  MmsGetNameListResponse decoded;
  bool moreFollowsPresent = false;
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedResponse(input, &pdu);
  if (!status.ok()) {
    return status;
  }
  if (pdu.invokeId != expectedInvokeId || pdu.serviceTag != 1) {
    return Invalid("GetNameList响应invokeID或服务选择不匹配");
  }
  std::size_t offset = 0;
  BerTlvView list;
  status = ReadBerTlv(pdu.serviceValue, &offset, &list);
  if (!status.ok() || !IsContextConstructed(list.tag, 0)) {
    return Invalid("GetNameList响应缺少Identifier列表");
  }
  auto remaining = list.value;
  while (!remaining.empty()) {
    if (decoded.identifiers.size() >= kMaxNameListEntries) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "GetNameList响应条目超过下位机上限");
    }
    std::size_t itemOffset = 0;
    BerTlvView identifier;
    status = ReadBerTlv(remaining, &itemOffset, &identifier);
    if (!status.ok() || identifier.tag != 0x1a || identifier.value.empty() ||
        identifier.value.size() > kMaxIdentifierBytes) {
      return Invalid("GetNameList响应Identifier错误");
    }
    decoded.identifiers.emplace_back(
        reinterpret_cast<const char*>(identifier.value.data()),
        identifier.value.size());
    remaining = remaining.subspan(itemOffset);
  }
  if (offset < pdu.serviceValue.size()) {
    BerTlvView more;
    status = ReadBerTlv(pdu.serviceValue, &offset, &more);
    if (!status.ok() || !IsContextPrimitive(more.tag, 1)) {
      return Invalid("GetNameList响应moreFollows字段错误");
    }
    status = ReadBerBoolean(more.value, &decoded.moreFollows);
    if (!status.ok()) {
      return Invalid("GetNameList响应moreFollows错误");
    }
    moreFollowsPresent = true;
  }
  if (!moreFollowsPresent || offset != pdu.serviceValue.size()) {
    return Invalid("GetNameList响应包含未知字段");
  }
  *response = std::move(decoded);
  return grpc::Status::OK;
}

grpc::Status EncodeMmsGetVariableAccessAttributesRequest(
    std::uint32_t invokeId, const MmsObjectName& objectName,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  std::vector<std::uint8_t> encodedName;
  auto status = EncodeObjectName(objectName, &encodedName);
  if (!status.ok()) {
    return status;
  }
  // GetVariableAccessAttributes使用VariableSpecification.name[0]选择；
  // 其内部才是VMD/Domain/AA对象名选择。
  std::vector<std::uint8_t> nameChoice;
  status = AppendTlv(0xa0, encodedName, &nameChoice);
  if (!status.ok()) {
    return status;
  }
  return EncodeMmsConfirmedRequest(invokeId, 6, nameChoice, output,
                                   outputSize);
}

grpc::Status DecodeMmsGetVariableAccessAttributesResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsGetVariableAccessAttributesResponse* response) {
  if (response == nullptr) {
    return ArgumentError("GetVariableAccessAttributes响应输出参数为空");
  }
  *response = {};
  MmsGetVariableAccessAttributesResponse decoded;
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedResponse(input, &pdu);
  if (!status.ok()) {
    return status;
  }
  if (pdu.invokeId != expectedInvokeId || pdu.serviceTag != 6) {
    return Invalid("GetVariableAccessAttributes响应invokeID或服务选择不匹配");
  }
  std::size_t offset = 0;
  BerTlvView deletable;
  status = ReadBerTlv(pdu.serviceValue, &offset, &deletable);
  if (!status.ok() || deletable.tag != 0x80) {
    return Invalid("GetVariableAccessAttributes响应缺少mmsDeletable");
  }
  status = ReadBerBoolean(deletable.value, &decoded.mmsDeletable);
  if (!status.ok()) {
    return Invalid("GetVariableAccessAttributes响应mmsDeletable无效");
  }
  BerTlvView typeSpecification;
  if (offset >= pdu.serviceValue.size()) {
    return Invalid("GetVariableAccessAttributes响应缺少TypeSpecification");
  }
  status = ReadBerTlv(pdu.serviceValue, &offset, &typeSpecification);
  if (!status.ok()) {
    return status;
  }
  if (typeSpecification.tag == 0xa1) {
    decoded.addressPresent = true;
    if (offset >= pdu.serviceValue.size()) {
      return Invalid("GetVariableAccessAttributes响应地址后缺少TypeSpecification");
    }
    status = ReadBerTlv(pdu.serviceValue, &offset, &typeSpecification);
  }
  if (!status.ok() || offset != pdu.serviceValue.size() ||
      typeSpecification.tag != 0xa2) {
    return Invalid("GetVariableAccessAttributes响应TypeSpecification字段错误");
  }
  std::size_t componentBudget = kMaxTypeSpecificationComponents;
  status = DecodeTypeSpecification(typeSpecification.value, 0,
                                    &componentBudget,
                                    &decoded.typeSpecification);
  if (!status.ok()) {
    return status;
  }
  *response = std::move(decoded);
  return grpc::Status::OK;
}

grpc::Status EncodeMmsGetNamedVariableListAttributesRequest(
    std::uint32_t invokeId, const MmsObjectName& objectName,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  std::vector<std::uint8_t> encodedName;
  auto status = EncodeObjectName(objectName, &encodedName);
  if (!status.ok()) {
    return status;
  }
  // variableListName直接使用ObjectName选择，不存在额外的[0]包装。
  return EncodeMmsConfirmedRequest(invokeId, 12, encodedName, output,
                                   outputSize);
}

grpc::Status DecodeMmsGetNamedVariableListAttributesResponse(
    std::span<const std::uint8_t> input, std::uint32_t expectedInvokeId,
    MmsGetNamedVariableListAttributesResponse* response) {
  if (response == nullptr) {
    return ArgumentError("GetNamedVariableListAttributes响应输出参数为空");
  }
  *response = {};
  MmsGetNamedVariableListAttributesResponse decoded;
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedResponse(input, &pdu);
  if (!status.ok()) {
    return status;
  }
  if (pdu.invokeId != expectedInvokeId || pdu.serviceTag != 12) {
    return Invalid(
        "GetNamedVariableListAttributes响应invokeID或服务选择不匹配");
  }
  std::size_t offset = 0;
  BerTlvView deletable;
  status = ReadBerTlv(pdu.serviceValue, &offset, &deletable);
  if (!status.ok() || deletable.tag != 0x80) {
    return Invalid("GetNamedVariableListAttributes响应缺少mmsDeletable");
  }
  status = ReadBerBoolean(deletable.value, &decoded.mmsDeletable);
  if (!status.ok()) {
    return Invalid("GetNamedVariableListAttributes响应mmsDeletable无效");
  }
  BerTlvView list;
  status = ReadBerTlv(pdu.serviceValue, &offset, &list);
  if (!status.ok() || offset != pdu.serviceValue.size() ||
      !IsContextConstructed(list.tag, 1)) {
    return Invalid(
        "GetNamedVariableListAttributes响应缺少listOfVariable");
  }
  std::size_t listOffset = 0;
  while (listOffset < list.value.size()) {
    if (decoded.variables.size() >= kMaxNameListEntries) {
      return grpc::Status(
          grpc::StatusCode::RESOURCE_EXHAUSTED,
          "GetNamedVariableListAttributes成员超过下位机上限");
    }
    BerTlvView variable;
    status = ReadBerTlv(list.value, &listOffset, &variable);
    if (!status.ok() || variable.tag != 0x30) {
      return Invalid(
          "GetNamedVariableListAttributes成员不是VariableList结构");
    }
    std::size_t variableOffset = 0;
    BerTlvView specification;
    status = ReadBerTlv(variable.value, &variableOffset, &specification);
    if (!status.ok() || variableOffset != variable.value.size() ||
        !IsContextConstructed(specification.tag, 0)) {
      return Invalid(
          "GetNamedVariableListAttributes成员缺少variableSpecification");
    }
    MmsObjectName objectName;
    status = DecodeObjectName(specification.value, &objectName);
    if (!status.ok()) {
      return status;
    }
    decoded.variables.emplace_back(std::move(objectName));
  }
  *response = std::move(decoded);
  return grpc::Status::OK;
}

grpc::Status EncodeMmsReadRequest(
    std::uint32_t invokeId, const MmsReadRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (request.variables.empty()) {
    return ArgumentError("Read请求至少需要一个变量访问项");
  }
  if (request.variables.size() > kMaxNameListEntries) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "Read请求变量访问项超过下位机上限");
  }

  std::vector<std::uint8_t> listContent;
  for (const auto& variable : request.variables) {
    std::vector<std::uint8_t> encodedName;
    auto status = EncodeObjectName(variable, &encodedName);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> specification;
    status = AppendTlv(0xa0, encodedName, &specification);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> item;
    status = AppendTlv(0x30, specification, &item);
    if (!status.ok()) {
      return status;
    }
    if (listContent.size() > kMaxMmsPduBytes - item.size()) {
      return OutputError("Read请求变量访问列表超过MMS上限");
    }
    listContent.insert(listContent.end(), item.begin(), item.end());
  }

  std::vector<std::uint8_t> list;
  // VariableAccessSpecification的listOfVariable选择是[0]；Read请求
  // 外层再使用[1]包裹该选择。
  auto status = AppendTlv(0xa0, listContent, &list);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> service;
  if (request.specificationWithResult) {
    const std::array<std::uint8_t, 3> trueValue{0x80, 0x01, 0xff};
    service.insert(service.end(), trueValue.begin(), trueValue.end());
  }
  std::vector<std::uint8_t> variableAccess;
  status = AppendTlv(0xa1, list, &variableAccess);
  if (!status.ok()) {
    return status;
  }
  service.insert(service.end(), variableAccess.begin(), variableAccess.end());
  return EncodeMmsConfirmedRequest(invokeId, 4, service, output, outputSize);
}

grpc::Status DecodeMmsReadRequest(std::span<const std::uint8_t> input,
                                  std::uint32_t* invokeId,
                                  MmsReadRequest* request) {
  if (invokeId == nullptr || request == nullptr) {
    return ArgumentError("Read请求输出参数为空");
  }
  *invokeId = 0;
  *request = {};
  std::uint32_t decodedInvokeId = 0;
  MmsReadRequest decoded;
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedRequest(input, &pdu);
  if (!status.ok()) {
    return status;
  }
  if (pdu.serviceTag != 4) {
    return Invalid("Confirmed请求不是Read");
  }
  decodedInvokeId = pdu.invokeId;
  std::size_t offset = 0;
  if (offset < pdu.serviceValue.size() &&
      IsContextPrimitive(pdu.serviceValue[offset], 0)) {
    BerTlvView specificationWithResult;
    status = ReadBerTlv(pdu.serviceValue, &offset, &specificationWithResult);
    if (!status.ok()) {
      return status;
    }
    status = ReadBerBoolean(specificationWithResult.value,
                            &decoded.specificationWithResult);
    if (!status.ok()) {
      return Invalid("Read请求specificationWithResult值错误");
    }
  }
  BerTlvView list;
  status = ReadBerTlv(pdu.serviceValue, &offset, &list);
  if (!status.ok() || !IsContextConstructed(list.tag, 1) ||
      list.value.empty()) {
    return Invalid("Read请求缺少listOfVariable");
  }
  std::size_t listOffset = 0;
  BerTlvView variableList;
  status = ReadBerTlv(list.value, &listOffset, &variableList);
  if (!status.ok() || listOffset != list.value.size() ||
      !IsContextConstructed(variableList.tag, 0) ||
      variableList.value.empty()) {
    return Invalid("Read请求listOfVariable选择错误");
  }
  listOffset = 0;
  while (listOffset < variableList.value.size()) {
    if (decoded.variables.size() >= kMaxNameListEntries) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "Read请求变量访问项超过下位机上限");
    }
    BerTlvView item;
    status = ReadBerTlv(variableList.value, &listOffset, &item);
    if (!status.ok() || item.tag != 0x30) {
      return Invalid("Read请求变量访问项不是SEQUENCE");
    }
    std::size_t itemOffset = 0;
    BerTlvView specification;
    status = ReadBerTlv(item.value, &itemOffset, &specification);
    if (!status.ok() || itemOffset != item.value.size() ||
        !IsContextConstructed(specification.tag, 0)) {
      return Invalid("Read请求变量访问项缺少variableSpecification");
    }
    MmsObjectName objectName;
    status = DecodeObjectName(specification.value, &objectName);
    if (!status.ok()) {
      return status;
    }
    decoded.variables.emplace_back(std::move(objectName));
  }
  if (offset != pdu.serviceValue.size()) {
    return Invalid("Read请求包含未知字段");
  }
  *invokeId = decodedInvokeId;
  *request = std::move(decoded);
  return grpc::Status::OK;
}

grpc::Status EncodeMmsReadResponse(
    std::uint32_t invokeId, const MmsReadResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (response.items.empty()) {
    return ArgumentError("Read响应至少需要一个响应项");
  }
  if (response.items.size() > kMaxNameListEntries) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "Read响应项超过下位机上限");
  }
  std::vector<std::uint8_t> listContent;
  for (const auto& item : response.items) {
    const auto& encoded = item.success ? item.encodedData : item.failure;
    const auto itemStatus = ValidateReadResponseItem(
        item.success,
        std::span<const std::uint8_t>(encoded.data(), encoded.size()));
    if (!itemStatus.ok()) {
      return itemStatus;
    }
    // ReadResponse的AccessResult成功项直接是Data选择，失败项直接是
    // [0] DataAccessError；这里不再添加额外的success/failure包装。
    if (encoded.size() > kMaxMmsPduBytes ||
        listContent.size() > kMaxMmsPduBytes - encoded.size()) {
      return OutputError("Read响应结果列表超过MMS上限");
    }
    listContent.insert(listContent.end(), encoded.begin(), encoded.end());
  }
  std::vector<std::uint8_t> service;
  auto status = AppendTlv(0xa1, listContent, &service);
  if (!status.ok()) {
    return status;
  }
  return EncodeMmsConfirmedResponse(invokeId, 4, service, output, outputSize);
}

grpc::Status DecodeMmsReadResponse(std::span<const std::uint8_t> input,
                                   std::uint32_t expectedInvokeId,
                                   MmsReadResponse* response) {
  if (response == nullptr) {
    return ArgumentError("Read响应输出参数为空");
  }
  *response = {};
  MmsReadResponse decoded;
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedResponse(input, &pdu);
  if (!status.ok()) {
    return status;
  }
  if (pdu.invokeId != expectedInvokeId || pdu.serviceTag != 4) {
    return Invalid("Read响应invokeID或服务选择不匹配");
  }
  std::size_t offset = 0;
  BerTlvView list;
  bool listPresent = false;
  bool specificationPresent = false;
  while (offset < pdu.serviceValue.size()) {
    BerTlvView field;
    status = ReadBerTlv(pdu.serviceValue, &offset, &field);
    if (!status.ok()) {
      return status;
    }
    if (IsContextConstructed(field.tag, 0)) {
      // specificationWithResult=true时，响应可选地返回变量访问规格；
      // 当前上层只需要结果值，但必须校验其CHOICE和对象名结构。
      if (listPresent || specificationPresent || field.value.empty()) {
        return Invalid("Read响应变量访问规格出现在结果列表之后");
      }
      std::size_t specificationOffset = 0;
      BerTlvView specification;
      status = ReadBerTlv(field.value, &specificationOffset, &specification);
      if (!status.ok() || specificationOffset != field.value.size() ||
          (!IsContextConstructed(specification.tag, 0) &&
           !IsContextConstructed(specification.tag, 1))) {
        return Invalid("Read响应变量访问规格CHOICE错误");
      }
      if (IsContextConstructed(specification.tag, 1)) {
        MmsObjectName objectName;
        status = DecodeObjectName(specification.value, &objectName);
        if (!status.ok()) {
          return Invalid("Read响应变量访问规格对象名错误");
        }
      } else {
        std::size_t variableOffset = 0;
        if (specification.value.empty()) {
          return Invalid("Read响应变量访问规格列表为空");
        }
        while (variableOffset < specification.value.size()) {
          BerTlvView item;
          status = ReadBerTlv(specification.value, &variableOffset, &item);
          if (!status.ok() || item.tag != 0x30) {
            return Invalid("Read响应变量访问规格成员不是SEQUENCE");
          }
          std::size_t itemOffset = 0;
          BerTlvView variableSpecification;
          status = ReadBerTlv(item.value, &itemOffset, &variableSpecification);
          if (!status.ok() || !IsContextConstructed(variableSpecification.tag, 0)) {
            return Invalid("Read响应变量访问规格成员缺少对象名");
          }
          MmsObjectName objectName;
          status = DecodeObjectName(variableSpecification.value, &objectName);
          if (!status.ok()) {
            return Invalid("Read响应变量访问规格成员对象名错误");
          }
          if (itemOffset < item.value.size()) {
            BerTlvView alternateAccess;
            status = ReadBerTlv(item.value, &itemOffset, &alternateAccess);
            if (!status.ok() || alternateAccess.tag != 0xa5 ||
                alternateAccess.value.empty()) {
              return Invalid("Read响应变量访问规格alternateAccess错误");
            }
          }
          if (itemOffset != item.value.size()) {
            return Invalid("Read响应变量访问规格成员包含未知字段");
          }
        }
      }
      specificationPresent = true;
      continue;
    }
    if (!IsContextConstructed(field.tag, 1) || listPresent) {
      return Invalid("Read响应结果列表选择错误");
    }
    list = field;
    listPresent = true;
  }
  if (!listPresent || list.value.empty()) {
    return Invalid("Read响应缺少结果列表");
  }
  std::size_t listOffset = 0;
  while (listOffset < list.value.size()) {
    if (decoded.items.size() >= kMaxNameListEntries) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "Read响应项超过下位机上限");
    }
    BerTlvView item;
    status = ReadBerTlv(list.value, &listOffset, &item);
    if (!status.ok()) {
      return status;
    }
    if (item.tag == 0x80) {
      std::int64_t failureCode = 0;
      status = ReadBerSigned(item.value, &failureCode);
      if (!status.ok() || failureCode < 0 || failureCode > 11) {
        return Invalid("Read响应DataAccessError值错误");
      }
    } else if (!IsMmsDataTag(item.tag)) {
      return Invalid("Read响应Data选择标签错误");
    }
    std::vector<std::uint8_t> encoded;
    status = AppendTlv(item.tag, item.value, &encoded);
    if (!status.ok()) {
      return status;
    }
    auto& decodedItem = decoded.items.emplace_back();
    decodedItem.success = item.tag != 0x80;
    if (decodedItem.success) {
      decodedItem.encodedData = std::move(encoded);
    } else {
      decodedItem.failure = std::move(encoded);
    }
  }
  *response = std::move(decoded);
  return grpc::Status::OK;
}

grpc::Status EncodeMmsDataBoolean(bool value,
                                  std::vector<std::uint8_t>* encodedData) {
  if (encodedData == nullptr) {
    return ArgumentError("MMS BOOLEAN Data输出参数为空");
  }
  const std::array<std::uint8_t, 1> raw{
      static_cast<std::uint8_t>(value ? 0xffu : 0x00u)};
  return AppendTlv(0x83, raw, encodedData);
}

grpc::Status EncodeMmsDataSigned(
    std::int64_t value, std::vector<std::uint8_t>* encodedData) {
  if (encodedData == nullptr) {
    return ArgumentError("MMS INTEGER Data输出参数为空");
  }
  encodedData->clear();
  return AppendSigned(0x85, value, encodedData);
}

grpc::Status EncodeMmsDataUnsigned(
    std::uint64_t value, std::vector<std::uint8_t>* encodedData) {
  return AppendUnsigned(0x86, value, encodedData);
}

grpc::Status EncodeMmsDataFloatingPoint(
    double value, std::uint8_t formatWidth,
    std::vector<std::uint8_t>* encodedData) {
  if (encodedData == nullptr) {
    return ArgumentError("MMS FLOATING-POINT Data输出参数为空");
  }
  encodedData->clear();
  if (formatWidth != 0x08 && formatWidth != 0x0b) {
    return ArgumentError("MMS FLOATING-POINT format-width必须为0x08或0x0B");
  }
  if (!std::isfinite(value)) {
    return ArgumentError("MMS FLOATING-POINT不允许非有限值");
  }

  std::array<std::uint8_t, 9> raw{};
  raw[0] = formatWidth;
  if (formatWidth == 0x08) {
    if (value > static_cast<double>(std::numeric_limits<float>::max()) ||
        value < -static_cast<double>(std::numeric_limits<float>::max())) {
      return ArgumentError("MMS FLOATING-POINT FLOAT32转换溢出");
    }
    const auto narrowed = static_cast<float>(value);
    if (!std::isfinite(narrowed)) {
      return ArgumentError("MMS FLOATING-POINT FLOAT32转换溢出");
    }
    const auto bits = std::bit_cast<std::uint32_t>(narrowed);
    raw[1] = static_cast<std::uint8_t>(bits >> 24);
    raw[2] = static_cast<std::uint8_t>(bits >> 16);
    raw[3] = static_cast<std::uint8_t>(bits >> 8);
    raw[4] = static_cast<std::uint8_t>(bits);
    return AppendTlv(0x87,
                     std::span<const std::uint8_t>(raw.data(), 5),
                     encodedData);
  }

  const auto bits = std::bit_cast<std::uint64_t>(value);
  for (std::size_t index = 0; index < sizeof(bits); ++index) {
    raw[index + 1] = static_cast<std::uint8_t>(
        bits >> ((sizeof(bits) - index - 1) * 8));
  }
  return AppendTlv(0x87,
                   std::span<const std::uint8_t>(raw.data(), raw.size()),
                   encodedData);
}

grpc::Status EncodeMmsDataBinaryTime(
    std::int64_t timestampMs, std::vector<std::uint8_t>* encodedData) {
  if (encodedData == nullptr) {
    return ArgumentError("MMS BinaryTime Data输出参数为空");
  }
  encodedData->clear();
  constexpr auto epoch1984Ms = kDaysFrom1984To1970 * kMillisecondsPerDay;
  if (timestampMs < epoch1984Ms) {
    return ArgumentError("MMS BinaryTime时间早于1984-01-01");
  }
  const auto elapsed = timestampMs - epoch1984Ms;
  const auto days = elapsed / kMillisecondsPerDay;
  const auto milliseconds = elapsed % kMillisecondsPerDay;
  if (days > std::numeric_limits<std::uint16_t>::max()) {
    return ArgumentError("MMS BinaryTime日期超出6字节范围");
  }
  std::array<std::uint8_t, 6> raw{
      static_cast<std::uint8_t>(days >> 8),
      static_cast<std::uint8_t>(days),
      static_cast<std::uint8_t>(milliseconds >> 24),
      static_cast<std::uint8_t>(milliseconds >> 16),
      static_cast<std::uint8_t>(milliseconds >> 8),
      static_cast<std::uint8_t>(milliseconds)};
  return AppendTlv(0x8c, raw, encodedData);
}

grpc::Status EncodeMmsDataUtcTime(
    std::int64_t timestampMs, bool timeQualityValid,
    std::vector<std::uint8_t>* encodedData) {
  if (encodedData == nullptr) {
    return ArgumentError("MMS UTC time Data输出参数为空");
  }
  encodedData->clear();
  if (timestampMs < 0) {
    return ArgumentError("MMS UTC time不能编码负时间戳");
  }
  const auto seconds = static_cast<std::uint64_t>(timestampMs / 1000);
  if (seconds > std::numeric_limits<std::uint32_t>::max()) {
    return ArgumentError("MMS UTC time秒值超出4字节范围");
  }
  const auto remainderMs = static_cast<std::uint64_t>(timestampMs % 1000);
  const auto fraction = (remainderMs << 24) / 1000;
  std::array<std::uint8_t, 8> raw{
      static_cast<std::uint8_t>(seconds >> 24),
      static_cast<std::uint8_t>(seconds >> 16),
      static_cast<std::uint8_t>(seconds >> 8),
      static_cast<std::uint8_t>(seconds),
      static_cast<std::uint8_t>(fraction >> 16),
      static_cast<std::uint8_t>(fraction >> 8),
      static_cast<std::uint8_t>(fraction),
      static_cast<std::uint8_t>(timeQualityValid ? 0x00 : 0x80)};
  return AppendTlv(0x91, raw, encodedData);
}

grpc::Status EncodeMmsDataVisibleString(
    std::string_view value, std::vector<std::uint8_t>* encodedData) {
  if (encodedData == nullptr) {
    return ArgumentError("MMS VisibleString Data输出参数为空");
  }
  if (value.size() > kMaxIdentifierBytes) {
    return ArgumentError("MMS VisibleString Data超过长度上限");
  }
  return AppendTlv(
      0x8a,
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(value.data()), value.size()),
      encodedData);
}

grpc::Status EncodeMmsDataBitString(
    std::uint8_t unusedBits, std::span<const std::uint8_t> payload,
    std::vector<std::uint8_t>* encodedData) {
  if (encodedData == nullptr) {
    return ArgumentError("MMS BIT STRING Data输出参数为空");
  }
  if (unusedBits > 7 || (payload.empty() && unusedBits != 0) ||
      unusedBits > payload.size() * 8) {
    return ArgumentError("MMS BIT STRING Data长度或未使用位数无效");
  }
  if (unusedBits != 0 &&
      (payload.back() & static_cast<std::uint8_t>((1u << unusedBits) - 1u)) !=
          0) {
    return ArgumentError("MMS BIT STRING Data未使用低位非零");
  }
  std::vector<std::uint8_t> raw;
  raw.reserve(payload.size() + 1);
  raw.push_back(unusedBits);
  raw.insert(raw.end(), payload.begin(), payload.end());
  return AppendTlv(0x84, raw, encodedData);
}

grpc::Status EncodeMmsWriteRequest(
    std::uint32_t invokeId, const MmsWriteRequest& request,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (request.items.empty()) {
    return ArgumentError("Write请求至少需要一个变量访问项");
  }
  if (request.items.size() > kMaxNameListEntries) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "Write请求变量访问项超过下位机上限");
  }

  std::vector<std::uint8_t> variableListContent;
  std::vector<std::uint8_t> dataListContent;
  for (const auto& item : request.items) {
    std::vector<std::uint8_t> encodedName;
    auto status = EncodeObjectName(item.variable, &encodedName);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> variableSpecification;
    status = AppendTlv(0xa0, encodedName, &variableSpecification);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> variable;
    status = AppendTlv(0x30, variableSpecification, &variable);
    if (!status.ok()) {
      return status;
    }
    if (variableListContent.size() > kMaxMmsPduBytes - variable.size()) {
      return OutputError("Write请求变量访问列表超过MMS上限");
    }
    variableListContent.insert(variableListContent.end(), variable.begin(),
                               variable.end());

    if (item.encodedData.empty()) {
      return ArgumentError("Write请求缺少Data选择");
    }
    std::size_t dataOffset = 0;
    BerTlvView data;
    status = ReadBerTlv(item.encodedData, &dataOffset, &data);
    if (!status.ok() || dataOffset != item.encodedData.size() ||
        !IsMmsDataTag(data.tag)) {
      return ArgumentError("Write请求Data选择或长度错误");
    }
    if (item.encodedData.size() > kMaxMmsPduBytes ||
        dataListContent.size() >
            kMaxMmsPduBytes - item.encodedData.size()) {
      return OutputError("Write请求Data列表超过MMS上限");
    }
    dataListContent.insert(dataListContent.end(), item.encodedData.begin(),
                           item.encodedData.end());
  }

  std::vector<std::uint8_t> variableAccess;
  auto status = AppendTlv(0xa0, variableListContent, &variableAccess);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> dataList;
  status = AppendTlv(0xa0, dataListContent, &dataList);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> service;
  service.reserve(variableAccess.size() + dataList.size());
  service.insert(service.end(), variableAccess.begin(), variableAccess.end());
  service.insert(service.end(), dataList.begin(), dataList.end());
  return EncodeMmsConfirmedRequest(invokeId, 5, service, output, outputSize);
}

grpc::Status DecodeMmsWriteRequest(std::span<const std::uint8_t> input,
                                   std::uint32_t* invokeId,
                                   MmsWriteRequest* request) {
  if (invokeId == nullptr || request == nullptr) {
    return ArgumentError("Write请求输出参数为空");
  }
  *invokeId = 0;
  *request = {};
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedRequest(input, &pdu);
  if (!status.ok()) {
    return status;
  }
  if (pdu.serviceTag != 5) {
    return Invalid("Confirmed请求不是Write");
  }
  // 先写入局部结果，完整校验变量和Data数量后再提交，避免失败时交付半成品。
  MmsWriteRequest decoded;
  std::size_t offset = 0;
  BerTlvView variableAccess;
  status = ReadBerTlv(pdu.serviceValue, &offset, &variableAccess);
  if (!status.ok() || !IsContextConstructed(variableAccess.tag, 0) ||
      variableAccess.value.empty()) {
    return Invalid("Write请求缺少listOfVariable");
  }
  std::size_t variableOffset = 0;
  while (variableOffset < variableAccess.value.size()) {
    if (decoded.items.size() >= kMaxNameListEntries) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "Write请求变量访问项超过下位机上限");
    }
    BerTlvView variable;
    status = ReadBerTlv(variableAccess.value, &variableOffset, &variable);
    if (!status.ok() || variable.tag != 0x30) {
      return Invalid("Write请求变量访问项不是SEQUENCE");
    }
    std::size_t itemOffset = 0;
    BerTlvView specification;
    status = ReadBerTlv(variable.value, &itemOffset, &specification);
    if (!status.ok() || itemOffset != variable.value.size() ||
        !IsContextConstructed(specification.tag, 0)) {
      return Invalid("Write请求变量访问项缺少对象名");
    }
    MmsObjectName objectName;
    status = DecodeObjectName(specification.value, &objectName);
    if (!status.ok()) {
      return status;
    }
    MmsWriteRequestItem item;
    item.variable = std::move(objectName);
    decoded.items.emplace_back(std::move(item));
  }

  BerTlvView dataList;
  status = ReadBerTlv(pdu.serviceValue, &offset, &dataList);
  if (!status.ok() || offset != pdu.serviceValue.size() ||
      !IsContextConstructed(dataList.tag, 0) || dataList.value.empty()) {
    return Invalid("Write请求缺少listOfData");
  }
  std::size_t dataOffset = 0;
  for (auto& item : decoded.items) {
    const auto dataBegin = dataOffset;
    BerTlvView data;
    status = ReadBerTlv(dataList.value, &dataOffset, &data);
    if (!status.ok() || !IsMmsDataTag(data.tag)) {
      return Invalid("Write请求包含无效Data选择");
    }
    item.encodedData.assign(dataList.value.begin() + dataBegin,
                            dataList.value.begin() + dataOffset);
  }
  if (dataOffset != dataList.value.size()) {
    return Invalid("Write请求变量和Data数量不一致");
  }
  *invokeId = pdu.invokeId;
  *request = std::move(decoded);
  return grpc::Status::OK;
}

grpc::Status EncodeMmsWriteResponse(
    std::uint32_t invokeId, const MmsWriteResponse& response,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (response.items.empty()) {
    return ArgumentError("Write响应至少需要一个结果项");
  }
  if (response.items.size() > kMaxNameListEntries) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "Write响应结果项超过下位机上限");
  }
  std::vector<std::uint8_t> service;
  for (const auto& item : response.items) {
    std::vector<std::uint8_t> result;
    grpc::Status status;
    if (item.success) {
      status = AppendTlv(0x81, {}, &result);
    } else {
      if (item.failureCode < 0 || item.failureCode > 11) {
        return ArgumentError("Write响应DataAccessError超出范围");
      }
      status = AppendSigned(0x80, item.failureCode, &result);
    }
    if (!status.ok()) {
      return status;
    }
    service.insert(service.end(), result.begin(), result.end());
  }
  return EncodeMmsConfirmedResponse(invokeId, 5, service, output,
                                    outputSize);
}

grpc::Status DecodeMmsWriteResponse(std::span<const std::uint8_t> input,
                                    std::uint32_t expectedInvokeId,
                                    MmsWriteResponse* response) {
  if (response == nullptr) {
    return ArgumentError("Write响应输出参数为空");
  }
  *response = {};
  MmsWriteResponse decoded;
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedResponse(input, &pdu);
  if (!status.ok()) {
    return status;
  }
  if (pdu.invokeId != expectedInvokeId || pdu.serviceTag != 5) {
    return Invalid("Write响应invokeID或服务选择不匹配");
  }
  if (pdu.serviceValue.empty()) {
    return Invalid("Write响应缺少结果项");
  }
  std::size_t offset = 0;
  while (offset < pdu.serviceValue.size()) {
    if (decoded.items.size() >= kMaxNameListEntries) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "Write响应结果项超过下位机上限");
    }
    BerTlvView result;
    status = ReadBerTlv(pdu.serviceValue, &offset, &result);
    if (!status.ok()) {
      return status;
    }
    if (result.tag == 0x81) {
      if (!result.value.empty()) {
        return Invalid("Write响应成功项不是NULL");
      }
      decoded.items.push_back({true, 0});
      continue;
    }
    if (result.tag != 0x80) {
      return Invalid("Write响应结果选择错误");
    }
    std::int64_t failureCode = 0;
    status = ReadBerSigned(result.value, &failureCode);
    if (!status.ok() || failureCode < 0 || failureCode > 11) {
      return Invalid("Write响应DataAccessError值错误");
    }
    decoded.items.push_back({false, failureCode});
  }
  *response = std::move(decoded);
  return grpc::Status::OK;
}

}  // namespace IEC61850
