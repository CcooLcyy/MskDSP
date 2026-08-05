#include "IEC61850MmsPdu.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "IEC61850MmsBer.h"

namespace IEC61850 {
namespace {

constexpr std::uint8_t kInitiateRequestTag = 0xa8;
constexpr std::uint8_t kInitiateResponseTag = 0xa9;
constexpr std::uint8_t kDetailTag = 0xa4;
constexpr std::size_t kMaxPduBytes = 4096;

grpc::Status Invalid(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::DATA_LOSS,
                      std::format("IEC61850 MMS Initiate报文无效: {}", reason));
}

grpc::Status OutputError(std::string_view reason) {
  return grpc::Status(
      grpc::StatusCode::RESOURCE_EXHAUSTED,
      std::format("IEC61850 MMS Initiate输出缓冲不足: {}", reason));
}

grpc::Status ArgumentError(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      std::format("IEC61850 MMS Initiate参数无效: {}", reason));
}

grpc::Status AppendTlv(std::uint8_t tag, std::span<const std::uint8_t> value,
                       std::vector<std::uint8_t>* output) {
  if (output == nullptr || value.size() > kMaxPduBytes) {
    return OutputError("字段长度超出上限");
  }
  const auto oldSize = output->size();
  output->resize(oldSize + value.size() + 16);
  BerWriter writer(std::span<std::uint8_t>(output->data() + oldSize,
                                           output->size() - oldSize));
  if (!writer.Tlv(tag, value)) {
    output->resize(oldSize);
    return OutputError("字段编码失败");
  }
  output->resize(oldSize + writer.size());
  return grpc::Status::OK;
}

grpc::Status EncodeInteger(std::uint8_t tag, std::uint64_t value,
                           std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return ArgumentError("整数输出参数为空");
  }
  std::array<std::uint8_t, sizeof(value) + 3> buffer{};
  BerWriter writer(buffer);
  if (!writer.Unsigned(tag, value)) {
    return OutputError("整数编码失败");
  }
  output->assign(writer.written().begin(), writer.written().end());
  return grpc::Status::OK;
}

grpc::Status ValidateBitString(const MmsBitString& value) {
  if (value.size == 0 || value.size > MmsBitString::kMaxBytes ||
      value.unusedBits > 7) {
    return ArgumentError("支持位串长度或未使用位数越界");
  }
  if (value.unusedBits != 0 &&
      (value.bytes[value.size - 1] &
       static_cast<std::uint8_t>((1u << value.unusedBits) - 1u)) != 0) {
    return ArgumentError("支持位串未使用低位必须为零");
  }
  return grpc::Status::OK;
}

grpc::Status EncodeBitString(std::uint8_t tag, const MmsBitString& value,
                             std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return ArgumentError("支持位串输出参数为空");
  }
  auto status = ValidateBitString(value);
  if (!status.ok()) {
    return status;
  }
  std::array<std::uint8_t, MmsBitString::kMaxBytes + 1> encoded{};
  encoded[0] = value.unusedBits;
  std::copy_n(value.bytes.begin(), value.size, encoded.begin() + 1);
  output->clear();
  return AppendTlv(tag,
                   std::span<const std::uint8_t>(encoded.data(), value.size + 1),
                   output);
}

grpc::Status EncodeDetail(std::uint8_t version, const MmsBitString& parameter,
                          const MmsBitString& serviceSupport,
                          std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return ArgumentError("Initiate detail输出参数为空");
  }
  if (version > 127) {
    return ArgumentError("Initiate版本号超出实现范围");
  }
  std::vector<std::uint8_t> content;
  auto status = EncodeInteger(0x80, version, &content);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> field;
  if (parameter.size != 2 || parameter.unusedBits != 5 ||
      serviceSupport.size != 11 || serviceSupport.unusedBits != 3) {
    return ArgumentError("Initiate支持位串长度必须为11位和85位");
  }
  status = EncodeBitString(0x81, parameter, &field);
  if (!status.ok()) {
    return status;
  }
  content.insert(content.end(), field.begin(), field.end());
  status = EncodeBitString(0x82, serviceSupport, &field);
  if (!status.ok()) {
    return status;
  }
  content.insert(content.end(), field.begin(), field.end());
  output->clear();
  return AppendTlv(kDetailTag, content, output);
}

grpc::Status EncodePdu(const MmsInitiateRequest& request,
                       std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return ArgumentError("请求输出参数为空");
  }
  if (!request.hasProposedMaxServOutstandingCalling ||
      !request.hasProposedMaxServOutstandingCalled) {
    return ArgumentError("Initiate请求缺少必需并发服务数字段");
  }
  if ((request.hasLocalDetailCalling && request.localDetailCalling > 0x7fffffffU) ||
      (request.hasProposedMaxServOutstandingCalling &&
       request.proposedMaxServOutstandingCalling > 0xffff) ||
      (request.hasProposedMaxServOutstandingCalled &&
       request.proposedMaxServOutstandingCalled > 0xffff) ||
      (request.hasProposedDataStructureNestingLevel &&
       request.proposedDataStructureNestingLevel > 0xff)) {
    return ArgumentError("Initiate请求整数超出协议范围");
  }
  output->clear();
  auto status = grpc::Status::OK;
  std::vector<std::uint8_t> field;
  if (request.hasLocalDetailCalling) {
    status = EncodeInteger(0x80, request.localDetailCalling, &field);
    if (!status.ok()) {
      return status;
    }
    output->insert(output->end(), field.begin(), field.end());
  }
  if (request.hasProposedMaxServOutstandingCalling) {
    status = EncodeInteger(0x81, request.proposedMaxServOutstandingCalling,
                           &field);
    if (!status.ok()) {
      return status;
    }
    output->insert(output->end(), field.begin(), field.end());
  }
  if (request.hasProposedMaxServOutstandingCalled) {
    status = EncodeInteger(0x82, request.proposedMaxServOutstandingCalled,
                           &field);
    if (!status.ok()) {
      return status;
    }
    output->insert(output->end(), field.begin(), field.end());
  }
  if (request.hasProposedDataStructureNestingLevel) {
    status = EncodeInteger(0x83, request.proposedDataStructureNestingLevel,
                           &field);
    if (!status.ok()) {
      return status;
    }
    output->insert(output->end(), field.begin(), field.end());
  }
  status = EncodeDetail(request.proposedVersionNumber,
                        request.proposedParameterSupport,
                        request.proposedServiceSupport, &field);
  if (!status.ok()) {
    return status;
  }
  output->insert(output->end(), field.begin(), field.end());
  std::vector<std::uint8_t> wrapped;
  status = AppendTlv(kInitiateRequestTag, *output, &wrapped);
  if (!status.ok()) {
    return status;
  }
  *output = std::move(wrapped);
  return grpc::Status::OK;
}

grpc::Status EncodePdu(const MmsInitiateResponse& response,
                       std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return ArgumentError("响应输出参数为空");
  }
  if (!response.hasNegotiatedMaxServOutstandingCalling ||
      !response.hasNegotiatedMaxServOutstandingCalled) {
    return ArgumentError("Initiate响应缺少必需并发服务数字段");
  }
  if ((response.hasLocalDetailCalled && response.localDetailCalled > 0x7fffffffU) ||
      (response.hasNegotiatedMaxServOutstandingCalling &&
       response.negotiatedMaxServOutstandingCalling > 0xffff) ||
      (response.hasNegotiatedMaxServOutstandingCalled &&
       response.negotiatedMaxServOutstandingCalled > 0xffff) ||
      (response.hasNegotiatedDataStructureNestingLevel &&
       response.negotiatedDataStructureNestingLevel > 0xff)) {
    return ArgumentError("Initiate响应整数超出协议范围");
  }
  output->clear();
  std::vector<std::uint8_t> field;
  auto status = grpc::Status::OK;
  if (response.hasLocalDetailCalled) {
    status = EncodeInteger(0x80, response.localDetailCalled, &field);
    if (!status.ok()) {
      return status;
    }
    output->insert(output->end(), field.begin(), field.end());
  }
  if (response.hasNegotiatedMaxServOutstandingCalling) {
    status = EncodeInteger(0x81, response.negotiatedMaxServOutstandingCalling,
                           &field);
    if (!status.ok()) {
      return status;
    }
    output->insert(output->end(), field.begin(), field.end());
  }
  if (response.hasNegotiatedMaxServOutstandingCalled) {
    status = EncodeInteger(0x82, response.negotiatedMaxServOutstandingCalled,
                           &field);
    if (!status.ok()) {
      return status;
    }
    output->insert(output->end(), field.begin(), field.end());
  }
  if (response.hasNegotiatedDataStructureNestingLevel) {
    status = EncodeInteger(0x83,
                           response.negotiatedDataStructureNestingLevel,
                           &field);
    if (!status.ok()) {
      return status;
    }
    output->insert(output->end(), field.begin(), field.end());
  }
  status = EncodeDetail(response.negotiatedVersionNumber,
                        response.negotiatedParameterSupport,
                        response.negotiatedServiceSupport, &field);
  if (!status.ok()) {
    return status;
  }
  output->insert(output->end(), field.begin(), field.end());
  std::vector<std::uint8_t> wrapped;
  status = AppendTlv(kInitiateResponseTag, *output, &wrapped);
  if (!status.ok()) {
    return status;
  }
  *output = std::move(wrapped);
  return grpc::Status::OK;
}

grpc::Status DecodeInteger(std::span<const std::uint8_t> value,
                           std::uint64_t maximum, std::uint64_t* result) {
  if (result == nullptr) {
    return ArgumentError("整数输出参数为空");
  }
  auto status = ReadBerUnsigned(value, result);
  if (!status.ok()) {
    return status;
  }
  if (*result > maximum) {
    return Invalid("整数超出协议范围");
  }
  return grpc::Status::OK;
}

grpc::Status DecodeBitString(std::span<const std::uint8_t> value,
                             MmsBitString* result) {
  if (result == nullptr) {
    return ArgumentError("支持位串输出参数为空");
  }
  *result = {};
  if (value.size() < 2 || value.size() - 1 > MmsBitString::kMaxBytes ||
      value.front() > 7) {
    return Invalid("支持位串长度或未使用位数越界");
  }
  const auto unusedBits = value.front();
  const auto byteCount = value.size() - 1;
  if (unusedBits != 0 &&
      (value.back() & static_cast<std::uint8_t>((1u << unusedBits) - 1u)) !=
          0) {
    return Invalid("支持位串包含非零未使用低位");
  }
  result->size = byteCount;
  result->unusedBits = unusedBits;
  std::copy_n(value.begin() + 1, byteCount, result->bytes.begin());
  return grpc::Status::OK;
}

grpc::Status DecodeDetail(std::span<const std::uint8_t> value,
                          std::uint8_t* version, MmsBitString* parameter,
                          MmsBitString* serviceSupport) {
  if (version == nullptr || parameter == nullptr || serviceSupport == nullptr) {
    return ArgumentError("Initiate detail输出参数为空");
  }
  bool hasVersion = false;
  bool hasParameter = false;
  bool hasServiceSupport = false;
  std::size_t offset = 0;
  while (offset < value.size()) {
    BerTlvView field;
    auto status = ReadBerTlv(value, &offset, &field);
    if (!status.ok()) {
      return status;
    }
    switch (field.tag) {
      case 0x80: {
        if (hasVersion) {
          return Invalid("Initiate detail重复版本号");
        }
        std::uint64_t decoded = 0;
        status = DecodeInteger(field.value, 127, &decoded);
        if (!status.ok()) {
          return status;
        }
        *version = static_cast<std::uint8_t>(decoded);
        hasVersion = true;
        break;
      }
      case 0x81:
        if (hasParameter) {
          return Invalid("Initiate detail重复参数支持位串");
        }
        status = DecodeBitString(field.value, parameter);
        if (!status.ok()) {
          return status;
        }
        hasParameter = true;
        break;
      case 0x82:
        if (hasServiceSupport) {
          return Invalid("Initiate detail重复服务支持位串");
        }
        status = DecodeBitString(field.value, serviceSupport);
        if (!status.ok()) {
          return status;
        }
        hasServiceSupport = true;
        break;
      default:
        return Invalid("Initiate detail包含不支持的字段");
    }
  }
  if (!hasVersion || !hasParameter || !hasServiceSupport) {
    return Invalid("Initiate detail缺少必需字段");
  }
  if (parameter->size != 2 || parameter->unusedBits != 5 ||
      serviceSupport->size != 11 || serviceSupport->unusedBits != 3) {
    return Invalid("Initiate支持位串不是11位和85位");
  }
  return grpc::Status::OK;
}

template <typename Result>
grpc::Status DecodeOuter(std::span<const std::uint8_t> input,
                         std::uint8_t expectedTag, Result* result) {
  if (result == nullptr) {
    return ArgumentError("Initiate输出参数为空");
  }
  std::size_t offset = 0;
  BerTlvView outer;
  auto status = ReadBerTlv(input, &offset, &outer);
  if (!status.ok() || offset != input.size() || outer.tag != expectedTag) {
    return Invalid("Initiate外层标签或长度无效");
  }
  bool hasDetail = false;
  bool hasFirst = false;
  bool hasSecond = false;
  bool hasThird = false;
  bool hasFourth = false;
  std::size_t fieldOffset = 0;
  while (fieldOffset < outer.value.size()) {
    BerTlvView field;
    status = ReadBerTlv(outer.value, &fieldOffset, &field);
    if (!status.ok()) {
      return status;
    }
    auto duplicate = [](bool present) {
      return present ? Invalid("Initiate字段重复") : grpc::Status::OK;
    };
    if constexpr (std::is_same_v<Result, MmsInitiateRequest>) {
      switch (field.tag) {
        case 0x80: {
          status = duplicate(hasFirst);
          std::uint64_t value = 0;
          if (status.ok()) {
            status = DecodeInteger(field.value, 0x7fffffffU, &value);
          }
          if (status.ok()) {
            result->localDetailCalling = static_cast<std::uint32_t>(value);
            result->hasLocalDetailCalling = true;
            hasFirst = true;
          }
          break;
        }
        case 0x81: {
          status = duplicate(hasSecond);
          std::uint64_t value = 0;
          if (status.ok()) {
            status = DecodeInteger(field.value, 0xffff, &value);
          }
          if (status.ok()) {
            result->proposedMaxServOutstandingCalling =
                static_cast<std::uint16_t>(value);
            result->hasProposedMaxServOutstandingCalling = true;
            hasSecond = true;
          }
          break;
        }
        case 0x82: {
          status = duplicate(hasThird);
          std::uint64_t value = 0;
          if (status.ok()) {
            status = DecodeInteger(field.value, 0xffff, &value);
          }
          if (status.ok()) {
            result->proposedMaxServOutstandingCalled =
                static_cast<std::uint16_t>(value);
            result->hasProposedMaxServOutstandingCalled = true;
            hasThird = true;
          }
          break;
        }
        case 0x83: {
          status = duplicate(hasFourth);
          std::uint64_t value = 0;
          if (status.ok()) {
            status = DecodeInteger(field.value, 0xff, &value);
          }
          if (status.ok()) {
            result->proposedDataStructureNestingLevel =
                static_cast<std::uint8_t>(value);
            result->hasProposedDataStructureNestingLevel = true;
            hasFourth = true;
          }
          break;
        }
        case kDetailTag:
          if (hasDetail) {
            status = Invalid("Initiate detail重复");
          } else {
            status = DecodeDetail(field.value, &result->proposedVersionNumber,
                                  &result->proposedParameterSupport,
                                  &result->proposedServiceSupport);
            hasDetail = status.ok();
          }
          break;
        default:
          status = Invalid("Initiate请求包含不支持的字段");
      }
    } else {
      switch (field.tag) {
        case 0x80: {
          status = duplicate(result->hasLocalDetailCalled);
          std::uint64_t value = 0;
          if (status.ok()) {
            status = DecodeInteger(field.value, 0x7fffffffU, &value);
          }
          if (status.ok()) {
            result->localDetailCalled = static_cast<std::uint32_t>(value);
            result->hasLocalDetailCalled = true;
          }
          break;
        }
        case 0x81: {
          status = duplicate(result->hasNegotiatedMaxServOutstandingCalling);
          std::uint64_t value = 0;
          if (status.ok()) {
            status = DecodeInteger(field.value, 0xffff, &value);
          }
          if (status.ok()) {
            result->negotiatedMaxServOutstandingCalling =
                static_cast<std::uint16_t>(value);
            result->hasNegotiatedMaxServOutstandingCalling = true;
          }
          break;
        }
        case 0x82: {
          status = duplicate(result->hasNegotiatedMaxServOutstandingCalled);
          std::uint64_t value = 0;
          if (status.ok()) {
            status = DecodeInteger(field.value, 0xffff, &value);
          }
          if (status.ok()) {
            result->negotiatedMaxServOutstandingCalled =
                static_cast<std::uint16_t>(value);
            result->hasNegotiatedMaxServOutstandingCalled = true;
          }
          break;
        }
        case 0x83: {
          status = duplicate(result->hasNegotiatedDataStructureNestingLevel);
          std::uint64_t value = 0;
          if (status.ok()) {
            status = DecodeInteger(field.value, 0xff, &value);
          }
          if (status.ok()) {
            result->negotiatedDataStructureNestingLevel =
                static_cast<std::uint8_t>(value);
            result->hasNegotiatedDataStructureNestingLevel = true;
          }
          break;
        }
        case kDetailTag:
          if (hasDetail) {
            status = Invalid("Initiate detail重复");
          } else {
            status = DecodeDetail(field.value, &result->negotiatedVersionNumber,
                                  &result->negotiatedParameterSupport,
                                  &result->negotiatedServiceSupport);
            hasDetail = status.ok();
          }
          break;
        default:
          status = Invalid("Initiate响应包含不支持的字段");
      }
    }
    if (!status.ok()) {
      return status;
    }
  }
  if constexpr (std::is_same_v<Result, MmsInitiateRequest>) {
    if (!hasSecond || !hasThird) {
      return Invalid("Initiate请求缺少必需并发服务数字段");
    }
  } else if (!result->hasNegotiatedMaxServOutstandingCalling ||
             !result->hasNegotiatedMaxServOutstandingCalled) {
    return Invalid("Initiate响应缺少必需并发服务数字段");
  }
  if (!hasDetail) {
    return Invalid("Initiate缺少detail字段");
  }
  return grpc::Status::OK;
}

template <typename Pdu>
grpc::Status EncodeToBuffer(const Pdu& pdu, std::uint8_t expectedTag,
                            std::span<std::uint8_t> output,
                            std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return ArgumentError("输出长度参数为空");
  }
  *outputSize = 0;
  std::vector<std::uint8_t> encoded;
  grpc::Status status;
  if constexpr (std::is_same_v<Pdu, MmsInitiateRequest>) {
    status = EncodePdu(pdu, &encoded);
  } else {
    status = EncodePdu(pdu, &encoded);
  }
  if (!status.ok()) {
    return status;
  }
  if (encoded.empty() || encoded.front() != expectedTag ||
      encoded.size() > output.size()) {
    return OutputError("Initiate报文");
  }
  std::copy(encoded.begin(), encoded.end(), output.begin());
  *outputSize = encoded.size();
  return grpc::Status::OK;
}

}  // namespace

grpc::Status EncodeMmsInitiateRequest(const MmsInitiateRequest& request,
                                      std::span<std::uint8_t> output,
                                      std::size_t* outputSize) {
  return EncodeToBuffer(request, kInitiateRequestTag, output, outputSize);
}

grpc::Status DecodeMmsInitiateRequest(std::span<const std::uint8_t> input,
                                      MmsInitiateRequest* request) {
  if (request == nullptr) {
    return ArgumentError("请求输出参数为空");
  }
  *request = {};
  request->hasLocalDetailCalling = false;
  request->hasProposedMaxServOutstandingCalling = false;
  request->hasProposedMaxServOutstandingCalled = false;
  request->hasProposedDataStructureNestingLevel = false;
  return DecodeOuter(input, kInitiateRequestTag, request);
}

grpc::Status EncodeMmsInitiateResponse(const MmsInitiateResponse& response,
                                       std::span<std::uint8_t> output,
                                       std::size_t* outputSize) {
  return EncodeToBuffer(response, kInitiateResponseTag, output, outputSize);
}

grpc::Status DecodeMmsInitiateResponse(
    std::span<const std::uint8_t> input, MmsInitiateResponse* response) {
  if (response == nullptr) {
    return ArgumentError("响应输出参数为空");
  }
  *response = {};
  response->hasLocalDetailCalled = false;
  response->hasNegotiatedMaxServOutstandingCalling = false;
  response->hasNegotiatedMaxServOutstandingCalled = false;
  response->hasNegotiatedDataStructureNestingLevel = false;
  return DecodeOuter(input, kInitiateResponseTag, response);
}

}  // namespace IEC61850
