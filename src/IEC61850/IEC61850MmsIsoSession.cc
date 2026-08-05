#include "IEC61850MmsIsoSession.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <string_view>
#include <vector>

#include "IEC61850MmsBer.h"

namespace IEC61850 {
namespace {

constexpr std::size_t kMaxSessionLength = 0xffff;
constexpr std::uint8_t kSessionUserDataParameter = 0xc1;
constexpr std::uint8_t kPresentationDataPdu = 0x61;
constexpr std::uint8_t kExternalTag = 0x28;
constexpr std::uint8_t kUserInformationTag = 0xbe;

grpc::Status Invalid(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::DATA_LOSS,
                      std::format("IEC61850 MMS ISO报文无效: {}", reason));
}

grpc::Status OutputError(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                      std::format("IEC61850 MMS ISO输出缓冲不足: {}", reason));
}

bool AppendByte(std::span<std::uint8_t> output, std::size_t* offset,
                std::uint8_t value) noexcept {
  if (offset == nullptr || *offset >= output.size()) {
    return false;
  }
  output[(*offset)++] = value;
  return true;
}

bool AppendBytes(std::span<std::uint8_t> output, std::size_t* offset,
                 std::span<const std::uint8_t> bytes) noexcept {
  if (offset == nullptr || bytes.size() > output.size() - *offset) {
    return false;
  }
  std::copy(bytes.begin(), bytes.end(), output.begin() + *offset);
  *offset += bytes.size();
  return true;
}

bool EncodeSessionLength(std::size_t value, std::span<std::uint8_t> output,
                         std::size_t* offset) noexcept {
  if (value > kMaxSessionLength || offset == nullptr) {
    return false;
  }
  if (value <= 254) {
    return AppendByte(output, offset, static_cast<std::uint8_t>(value));
  }
  return AppendByte(output, offset, 0xff) &&
         AppendByte(output, offset, static_cast<std::uint8_t>(value >> 8)) &&
         AppendByte(output, offset, static_cast<std::uint8_t>(value));
}

bool DecodeSessionLength(std::span<const std::uint8_t> input,
                         std::size_t* offset, std::size_t* value) noexcept {
  if (offset == nullptr || value == nullptr || *offset >= input.size()) {
    return false;
  }
  const auto first = input[(*offset)++];
  if (first <= 254) {
    *value = first;
    return true;
  }
  if (first != 0xff || input.size() - *offset < 2) {
    return false;
  }
  *value = (static_cast<std::size_t>(input[(*offset)++]) << 8) |
           input[(*offset)++];
  return true;
}

bool AppendSessionParameter(std::span<std::uint8_t> output,
                            std::size_t* offset, std::uint8_t parameter,
                            std::span<const std::uint8_t> value) noexcept {
  return AppendByte(output, offset, parameter) &&
         EncodeSessionLength(value.size(), output, offset) &&
         AppendBytes(output, offset, value);
}

grpc::Status EncodeSessionWithUserData(
    IsoSessionPduType type, std::span<const std::uint8_t> userData,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS ISO输出长度参数为空");
  }
  *outputSize = 0;
  if (userData.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS ISO用户数据不能为空");
  }
  std::array<std::uint8_t, 12> connectParameters{
      0x05, 0x06, 0x13, 0x01, 0x00, 0x16,
      0x01, 0x00, 0x14, 0x02, 0x00, 0x02};
  const auto hasConnectParameters = type == IsoSessionPduType::CONNECT ||
                                    type == IsoSessionPduType::ACCEPT;
  const auto fixedSize = hasConnectParameters ? connectParameters.size() : 0;
  const auto userParameterSize = userData.size() <= 254 ? 2 : 4;
  const auto totalLength = fixedSize + userParameterSize + userData.size();
  const auto lengthFieldSize = totalLength <= 254 ? 1 : 3;
  if (fixedSize > kMaxSessionLength ||
      userData.size() > kMaxSessionLength - fixedSize - userParameterSize ||
      output.size() < 1 + lengthFieldSize + totalLength) {
    return OutputError("会话用户数据过大");
  }

  std::size_t offset = 1;
  if (!EncodeSessionLength(totalLength, output, &offset)) {
    return OutputError("会话长度编码失败");
  }
  if (hasConnectParameters &&
      !AppendBytes(output, &offset, connectParameters)) {
    return OutputError("连接参数写入失败");
  }
  if (!AppendSessionParameter(output, &offset, kSessionUserDataParameter,
                              userData)) {
    return OutputError("会话用户数据写入失败");
  }
  if (offset != 1 + (totalLength <= 254 ? 1 : 3) + totalLength) {
    return OutputError("会话长度计算不一致");
  }
  output[0] = static_cast<std::uint8_t>(type);
  *outputSize = offset;
  return grpc::Status::OK;
}

grpc::Status EncodeSessionDataInternal(
    std::span<const std::uint8_t> userData, std::span<std::uint8_t> output,
    std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS ISO输出长度参数为空");
  }
  *outputSize = 0;
  if (userData.empty() || output.size() < 4 ||
      userData.size() > output.size() - 4) {
    return OutputError("DATA用户数据过大或为空");
  }
  output[0] = static_cast<std::uint8_t>(IsoSessionPduType::DATA);
  output[1] = 0;
  output[2] = static_cast<std::uint8_t>(IsoSessionPduType::DATA);
  output[3] = 0;
  std::copy(userData.begin(), userData.end(), output.begin() + 4);
  *outputSize = userData.size() + 4;
  return grpc::Status::OK;
}

grpc::Status EncodeBerTlv(std::uint8_t tag,
                          std::span<const std::uint8_t> value,
                          std::vector<std::uint8_t>* output) {
  if (output == nullptr || value.size() > kMaxSessionLength) {
    return OutputError("BER字段过大");
  }
  output->assign(value.size() + 16, 0);
  BerWriter writer(*output);
  if (!writer.Tlv(tag, value)) {
    output->clear();
    return OutputError("BER字段写入失败");
  }
  output->resize(writer.size());
  return grpc::Status::OK;
}

grpc::Status EncodeBerInteger(std::uint8_t tag, std::uint32_t value,
                              std::vector<std::uint8_t>* output) {
  output->assign(8, 0);
  BerWriter writer(*output);
  if (!writer.Unsigned(tag, value)) {
    output->clear();
    return OutputError("BER整数写入失败");
  }
  output->resize(writer.size());
  return grpc::Status::OK;
}

grpc::Status EncodeOid(std::uint8_t tag, std::span<const std::uint32_t> oid,
                       std::vector<std::uint8_t>* output) {
  output->assign(256, 0);
  BerWriter writer(*output);
  if (!writer.Oid(tag, oid)) {
    output->clear();
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS OID无效或过长");
  }
  output->resize(writer.size());
  return grpc::Status::OK;
}

grpc::Status EncodeExternalUserInformation(
    std::span<const std::uint8_t> mmsPdu, std::vector<std::uint8_t>* output) {
  if (mmsPdu.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS user-information不能为空");
  }
  std::vector<std::uint8_t> oid;
  auto status = EncodeOid(0x06, kMmsAbstractSyntaxOid, &oid);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> single;
  status = EncodeBerTlv(0xa0, mmsPdu, &single);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> externalContent;
  externalContent.reserve(oid.size() + single.size());
  externalContent.insert(externalContent.end(), oid.begin(), oid.end());
  externalContent.insert(externalContent.end(), single.begin(), single.end());
  std::vector<std::uint8_t> external;
  status = EncodeBerTlv(kExternalTag, externalContent, &external);
  if (!status.ok()) {
    return status;
  }
  return EncodeBerTlv(kUserInformationTag, external, output);
}

grpc::Status EncodeAcsePdu(
    std::uint8_t applicationTag,
    std::span<const std::uint32_t> applicationContextOid,
    std::span<const std::uint8_t> mmsPdu, std::uint32_t result,
    bool includeResult, std::vector<std::uint8_t>* output) {
  if (applicationContextOid.size() < 2 || applicationContextOid.size() > 16) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS application-context OID无效");
  }
  std::vector<std::uint8_t> content;
  std::vector<std::uint8_t> field;
  auto status = EncodeBerTlv(0x80, std::array<std::uint8_t, 2>{0x07, 0x80},
                             &field);
  if (!status.ok()) {
    return status;
  }
  content.insert(content.end(), field.begin(), field.end());

  std::vector<std::uint8_t> oid;
  status = EncodeOid(0x06, applicationContextOid, &oid);
  if (!status.ok()) {
    return status;
  }
  status = EncodeBerTlv(0xa1, oid, &field);
  if (!status.ok()) {
    return status;
  }
  content.insert(content.end(), field.begin(), field.end());

  if (includeResult) {
    std::vector<std::uint8_t> integer;
    status = EncodeBerInteger(0x02, result, &integer);
    if (!status.ok()) {
      return status;
    }
    status = EncodeBerTlv(0xa2, integer, &field);
    if (!status.ok()) {
      return status;
    }
    content.insert(content.end(), field.begin(), field.end());

    std::vector<std::uint8_t> diagnosticInteger;
    status = EncodeBerInteger(0x02, 0, &diagnosticInteger);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::uint8_t> diagnosticSource;
    status = EncodeBerTlv(0xa1, diagnosticInteger, &diagnosticSource);
    if (!status.ok()) {
      return status;
    }
    status = EncodeBerTlv(0xa3, diagnosticSource, &field);
    if (!status.ok()) {
      return status;
    }
    content.insert(content.end(), field.begin(), field.end());
  }

  if (!mmsPdu.empty()) {
    std::vector<std::uint8_t> userInformation;
    status = EncodeExternalUserInformation(mmsPdu, &userInformation);
    if (!status.ok()) {
      return status;
    }
    content.insert(content.end(), userInformation.begin(),
                   userInformation.end());
  }
  return EncodeBerTlv(applicationTag, content, output);
}

grpc::Status DecodeUserInformation(std::span<const std::uint8_t> value,
                                   std::span<const std::uint8_t>* mmsPdu) {
  if (mmsPdu == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS user-information输出参数为空");
  }
  *mmsPdu = {};
  std::size_t offset = 0;
  BerTlvView encoded;
  auto status = ReadBerTlv(value, &offset, &encoded);
  if (!status.ok() || offset != value.size()) {
    return Invalid("user-information外层结构无效");
  }
  if (encoded.tag == 0x04) {
    *mmsPdu = encoded.value;
    return grpc::Status::OK;
  }
  if (encoded.tag != kExternalTag) {
    return Invalid("user-information不是EXTERNAL或OCTET STRING");
  }
  offset = 0;
  BerTlvView directReference;
  status = ReadBerTlv(encoded.value, &offset, &directReference);
  if (!status.ok() || directReference.tag != 0x06) {
    return Invalid("EXTERNAL缺少抽象语法OID");
  }
  std::array<std::uint32_t, 16> arcs{};
  std::size_t arcCount = 0;
  status = ReadBerOid(directReference.value, arcs, &arcCount);
  if (!status.ok() || arcCount != kMmsAbstractSyntaxOid.size() ||
      !std::equal(arcs.begin(), arcs.begin() + arcCount,
                  kMmsAbstractSyntaxOid.begin())) {
    return Invalid("EXTERNAL抽象语法OID不是MMS");
  }
  BerTlvView single;
  status = ReadBerTlv(encoded.value, &offset, &single);
  if (!status.ok() || single.tag != 0xa0 || offset != encoded.value.size()) {
    return Invalid("EXTERNAL缺少single-ASN1-type");
  }
  *mmsPdu = single.value;
  return grpc::Status::OK;
}

grpc::Status DecodeAcsePdu(std::span<const std::uint8_t> input,
                           std::uint8_t expectedTag, MmsAareView* result) {
  if (result == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS ACSE输出参数为空");
  }
  *result = {};
  std::size_t offset = 0;
  BerTlvView outer;
  auto status = ReadBerTlv(input, &offset, &outer);
  if (!status.ok() || offset != input.size() || outer.tag != expectedTag) {
    return Invalid("ACSE应用标签或长度无效");
  }
  bool hasContext = false;
  bool hasResult = expectedTag == 0x61 ? false : true;
  std::size_t innerOffset = 0;
  while (innerOffset < outer.value.size()) {
    BerTlvView field;
    status = ReadBerTlv(outer.value, &innerOffset, &field);
    if (!status.ok()) {
      return status;
    }
    switch (field.tag) {
      case 0x80:
        if (field.value.size() != 2 || field.value[0] != 0x07 ||
            field.value[1] != 0x80) {
          return Invalid("ACSE协议版本不是版本1");
        }
        break;
      case 0xa1: {
        if (hasContext) {
          return Invalid("ACSE重复application-context-name");
        }
        hasContext = true;
        std::size_t oidOffset = 0;
        BerTlvView oid;
        status = ReadBerTlv(field.value, &oidOffset, &oid);
        if (!status.ok() || oid.tag != 0x06 || oidOffset != field.value.size()) {
          return Invalid("ACSE application-context-name无效");
        }
        status = ReadBerOid(oid.value, result->applicationContextOid,
                            &result->applicationContextOidSize);
        if (!status.ok()) {
          return status;
        }
        break;
      }
      case 0xa2: {
        if (expectedTag != 0x61 || hasResult) {
          return Invalid("AARE result字段重复或位置无效");
        }
        hasResult = true;
        std::size_t integerOffset = 0;
        BerTlvView integer;
        status = ReadBerTlv(field.value, &integerOffset, &integer);
        if (!status.ok() || integer.tag != 0x02 ||
            integerOffset != field.value.size()) {
          return Invalid("AARE result不是INTEGER");
        }
        std::uint64_t value = 0;
        status = ReadBerUnsigned(integer.value, &value);
        if (!status.ok() || value > std::numeric_limits<std::uint32_t>::max()) {
          return Invalid("AARE result超出范围");
        }
        result->result = static_cast<std::uint32_t>(value);
        break;
      }
      case kUserInformationTag: {
        if (!result->mmsPdu.empty()) {
          return Invalid("ACSE重复user-information");
        }
        status = DecodeUserInformation(field.value, &result->mmsPdu);
        if (!status.ok()) {
          return status;
        }
        break;
      }
      default:
        // 其它ACSE可选字段不参与本期MMS客户端协商，但必须保持TLV完整。
        break;
    }
  }
  if (!hasContext || (expectedTag == 0x61 && !hasResult)) {
    return Invalid("ACSE缺少必需字段");
  }
  return grpc::Status::OK;
}

}  // namespace

grpc::Status EncodeIsoSessionConnect(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeSessionWithUserData(IsoSessionPduType::CONNECT,
                                   presentationData, output, outputSize);
}

grpc::Status EncodeIsoSessionAccept(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeSessionWithUserData(IsoSessionPduType::ACCEPT,
                                   presentationData, output, outputSize);
}

grpc::Status EncodeIsoSessionData(std::span<const std::uint8_t> presentationData,
                                  std::span<std::uint8_t> output,
                                  std::size_t* outputSize) {
  return EncodeSessionDataInternal(presentationData, output, outputSize);
}

grpc::Status EncodeIsoSessionFinish(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeSessionWithUserData(IsoSessionPduType::FINISH,
                                   presentationData, output, outputSize);
}

grpc::Status EncodeIsoSessionDisconnect(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeSessionWithUserData(IsoSessionPduType::DISCONNECT,
                                   presentationData, output, outputSize);
}

grpc::Status EncodeIsoSessionAbort(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS ISO输出长度参数为空");
  }
  *outputSize = 0;
  const auto userParameterSize = presentationData.empty()
                                     ? 0
                                     : 2 + presentationData.size();
  const auto totalLength = 3 + userParameterSize;
  const auto lengthFieldSize = totalLength <= 254 ? 1 : 3;
  if (totalLength > kMaxSessionLength ||
      output.size() < 1 + lengthFieldSize + totalLength) {
    return OutputError("ABORT用户数据过大");
  }
  std::size_t offset = 1;
  if (!EncodeSessionLength(totalLength, output, &offset) ||
      !AppendByte(output, &offset, 0x11) ||
      !AppendByte(output, &offset, 0x01) ||
      !AppendByte(output, &offset, 0x03)) {
    return OutputError("ABORT原因字段写入失败");
  }
  if (!presentationData.empty() &&
      !AppendSessionParameter(output, &offset, kSessionUserDataParameter,
                              presentationData)) {
    return OutputError("ABORT用户数据写入失败");
  }
  output[0] = static_cast<std::uint8_t>(IsoSessionPduType::ABORT);
  *outputSize = offset;
  return grpc::Status::OK;
}

grpc::Status DecodeIsoSessionPdu(std::span<const std::uint8_t> input,
                                 IsoSessionPduView* pdu) {
  if (pdu == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS ISO会话输出参数为空");
  }
  *pdu = {};
  if (input.size() < 2) {
    return Invalid("会话SPDU长度不足");
  }
  const auto type = static_cast<IsoSessionPduType>(input.front());
  std::size_t offset = 1;
  if (type == IsoSessionPduType::DATA) {
    if (input.size() < 4 || input[1] != 0 || input[2] != 1 || input[3] != 0) {
      return Invalid("DATA SPDU缺少标准Token和参数字段");
    }
    pdu->type = type;
    pdu->userData = input.subspan(4);
    if (pdu->userData.empty()) {
      return Invalid("DATA SPDU用户数据为空");
    }
    return grpc::Status::OK;
  }
  std::size_t totalLength = 0;
  if (!DecodeSessionLength(input, &offset, &totalLength) ||
      totalLength != input.size() - offset) {
    return Invalid("SPDU长度字段与报文不一致");
  }
  std::size_t userDataOffset = offset;
  while (userDataOffset < input.size()) {
    const auto parameter = input[userDataOffset++];
    std::size_t parameterLength = 0;
    if (!DecodeSessionLength(input, &userDataOffset, &parameterLength) ||
        parameterLength > input.size() - userDataOffset) {
      return Invalid("SPDU参数长度无效");
    }
    const auto value = input.subspan(userDataOffset, parameterLength);
    if (parameter == kSessionUserDataParameter) {
      if (!pdu->userData.empty()) {
        return Invalid("SPDU重复用户数据参数");
      }
      pdu->userData = value;
    }
    userDataOffset += parameterLength;
  }
  switch (type) {
    case IsoSessionPduType::CONNECT:
    case IsoSessionPduType::ACCEPT:
    case IsoSessionPduType::FINISH:
    case IsoSessionPduType::DISCONNECT:
      if (pdu->userData.empty()) {
        return Invalid("SPDU缺少用户数据参数");
      }
      pdu->type = type;
      return grpc::Status::OK;
    case IsoSessionPduType::ABORT:
      pdu->type = type;
      return grpc::Status::OK;
    default:
      return Invalid("不支持的Session SPDU类型");
  }
}

grpc::Status EncodeMmsAarq(
    std::span<const std::uint32_t> applicationContextOid,
    std::span<const std::uint8_t> mmsPdu, std::span<std::uint8_t> output,
    std::size_t* outputSize) {
  std::vector<std::uint8_t> encoded;
  const auto status = EncodeAcsePdu(0x60, applicationContextOid, mmsPdu, 0,
                                    false, &encoded);
  if (!status.ok()) {
    if (outputSize != nullptr) {
      *outputSize = 0;
    }
    return status;
  }
  if (outputSize == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS AARQ输出长度参数为空");
  }
  *outputSize = 0;
  if (encoded.size() > output.size()) {
    return OutputError("AARQ报文");
  }
  std::copy(encoded.begin(), encoded.end(), output.begin());
  *outputSize = encoded.size();
  return grpc::Status::OK;
}

grpc::Status EncodeMmsAare(
    std::span<const std::uint32_t> applicationContextOid,
    std::uint32_t result, std::span<const std::uint8_t> mmsPdu,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  std::vector<std::uint8_t> encoded;
  const auto status = EncodeAcsePdu(0x61, applicationContextOid, mmsPdu,
                                    result, true, &encoded);
  if (!status.ok()) {
    if (outputSize != nullptr) {
      *outputSize = 0;
    }
    return status;
  }
  if (outputSize == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS AARE输出长度参数为空");
  }
  *outputSize = 0;
  if (encoded.size() > output.size()) {
    return OutputError("AARE报文");
  }
  std::copy(encoded.begin(), encoded.end(), output.begin());
  *outputSize = encoded.size();
  return grpc::Status::OK;
}

grpc::Status DecodeMmsAare(std::span<const std::uint8_t> input,
                           MmsAareView* result) {
  return DecodeAcsePdu(input, 0x61, result);
}

grpc::Status DecodeMmsAarq(std::span<const std::uint8_t> input,
                           MmsAareView* result) {
  return DecodeAcsePdu(input, 0x60, result);
}

grpc::Status EncodeMmsPresentationData(std::span<const std::uint8_t> mmsPdu,
                                        std::span<std::uint8_t> output,
                                        std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS P-DATA输出长度参数为空");
  }
  *outputSize = 0;
  if (mmsPdu.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS P-DATA用户数据不能为空");
  }
  std::vector<std::uint8_t> single;
  auto status = EncodeBerTlv(0xa0, mmsPdu, &single);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> contextId{0x02, 0x01, 0x03};
  std::vector<std::uint8_t> sequence;
  sequence.reserve(contextId.size() + single.size());
  sequence.insert(sequence.end(), contextId.begin(), contextId.end());
  sequence.insert(sequence.end(), single.begin(), single.end());
  std::vector<std::uint8_t> pdv;
  status = EncodeBerTlv(0x30, sequence, &pdv);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> presentation;
  status = EncodeBerTlv(kPresentationDataPdu, pdv, &presentation);
  if (!status.ok()) {
    return status;
  }
  if (presentation.size() > output.size()) {
    return OutputError("P-DATA报文");
  }
  std::copy(presentation.begin(), presentation.end(), output.begin());
  *outputSize = presentation.size();
  return grpc::Status::OK;
}

grpc::Status DecodeMmsPresentationData(std::span<const std::uint8_t> input,
                                        std::span<const std::uint8_t>* mmsPdu) {
  if (mmsPdu == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS P-DATA输出参数为空");
  }
  *mmsPdu = {};
  std::size_t offset = 0;
  BerTlvView outer;
  auto status = ReadBerTlv(input, &offset, &outer);
  if (!status.ok() || offset != input.size() || outer.tag != kPresentationDataPdu) {
    return Invalid("P-DATA-TF外层结构无效");
  }
  offset = 0;
  BerTlvView sequence;
  status = ReadBerTlv(outer.value, &offset, &sequence);
  if (!status.ok() || offset != outer.value.size() || sequence.tag != 0x30) {
    return Invalid("P-DATA-TF缺少PDV列表");
  }
  offset = 0;
  BerTlvView contextId;
  status = ReadBerTlv(sequence.value, &offset, &contextId);
  if (!status.ok() || contextId.tag != 0x02) {
    return Invalid("P-DATA-TF缺少Presentation Context ID");
  }
  std::uint64_t context = 0;
  status = ReadBerUnsigned(contextId.value, &context);
  if (!status.ok() || context != 3) {
    return Invalid("P-DATA-TF不是MMS Presentation Context");
  }
  BerTlvView single;
  status = ReadBerTlv(sequence.value, &offset, &single);
  if (!status.ok() || offset != sequence.value.size() || single.tag != 0xa0) {
    return Invalid("P-DATA-TF缺少single-ASN1-type");
  }
  *mmsPdu = single.value;
  return grpc::Status::OK;
}

}  // namespace IEC61850
