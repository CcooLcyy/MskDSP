#include "IEC61850MmsIsoSession.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "IEC61850MmsBer.h"

namespace IEC61850 {
namespace {

constexpr std::size_t kMaxSessionLength = 0xffff;
constexpr std::uint8_t kSessionUserDataParameter = 0xc1;
constexpr std::uint8_t kPresentationDataPdu = 0x61;
constexpr std::uint8_t kExternalTag = 0x28;
constexpr std::uint8_t kUserInformationTag = 0xbe;
// 会话层CONNECT/ACCEPT连接项参数：会话要求、版本号与会话选择子。
constexpr std::uint8_t kSessionRequirementParameter = 0x05;
constexpr std::uint8_t kVersionNumberParameter = 0x16;
constexpr std::uint8_t kCallingSessionSelectorParameter = 0x33;
constexpr std::uint8_t kCalledSessionSelectorParameter = 0x34;
// ISO 8327要求的版本号；实测装置只接受02，写00会被直接ABORT。
constexpr std::uint8_t kSessionVersionNumber = 0x02;
// 表示层CP子结构：mode-selector、normal-mode-parameters、上下文定义列表。
constexpr std::uint8_t kModeSelectorTag = 0xa0;
constexpr std::uint8_t kNormalModeParametersTag = 0xa2;
constexpr std::uint8_t kCallingPresentationSelectorTag = 0x81;
constexpr std::uint8_t kCalledPresentationSelectorTag = 0x82;
constexpr std::uint8_t kContextDefinitionListTag = 0xa4;
constexpr std::uint8_t kSingleAsn1TypeTag = 0xa0;
constexpr std::uint8_t kIndirectReferenceTag = 0x02;
// EXTERNAL中的indirect-reference必须指向CP里MMS表示层上下文的编号。
constexpr std::uint32_t kMmsIndirectReference = 3;

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

// 把一条会话参数编码到buffer的指定子区间，返回该参数的字节长度。
bool BuildSessionParameter(std::uint8_t parameter,
                           std::span<const std::uint8_t> value,
                           std::span<std::uint8_t> buffer,
                           std::size_t* size) noexcept {
  if (size == nullptr) {
    return false;
  }
  *size = 0;
  return AppendSessionParameter(buffer, size, parameter, value);
}

// 组装CONNECT/ACCEPT的连接项参数：会话要求+版本号+调用/被调会话选择子。
// 参数顺序与实测被装置接受的报文一致，连接项内部不得再嵌套一层TLV。
bool BuildConnectParameters(const IsoSessionSelectors& selectors,
                            std::array<std::uint8_t, 32>* parameters,
                            std::size_t* size) {
  if (parameters == nullptr || size == nullptr || selectors.callingSize == 0 ||
      selectors.callingSize > IsoSessionSelectors::kMaxSelectorBytes ||
      selectors.calledSize == 0 ||
      selectors.calledSize > IsoSessionSelectors::kMaxSelectorBytes) {
    return false;
  }
  const std::array<std::uint8_t, 2> requirement{0x00, 0x02};
  const std::array<std::uint8_t, 1> version{kSessionVersionNumber};
  const std::array<std::pair<std::uint8_t, std::span<const std::uint8_t>>, 4>
      fields{{
          {kSessionRequirementParameter, requirement},
          {kVersionNumberParameter, version},
          {kCallingSessionSelectorParameter,
           std::span<const std::uint8_t>(selectors.calling.data(),
                                         selectors.callingSize)},
          {kCalledSessionSelectorParameter,
           std::span<const std::uint8_t>(selectors.called.data(),
                                         selectors.calledSize)},
      }};
  std::size_t offset = 0;
  for (const auto& [parameter, value] : fields) {
    std::size_t fieldSize = 0;
    if (!BuildSessionParameter(
            parameter, value,
            std::span<std::uint8_t>(parameters->data() + offset,
                                    parameters->size() - offset),
            &fieldSize)) {
      return false;
    }
    offset += fieldSize;
  }
  *size = offset;
  return true;
}

// 校验对端CONNECT/ACCEPT连接项：必须存在会话要求(0x13)且版本号(0x16)为02；
// 会话选择子(0x33/0x34)按实际报文校验，缺失时只提示不拒绝，保证对不同实现宽容。
grpc::Status ValidateConnectParameters(std::span<const std::uint8_t> parameters,
                                       bool requireSessionSelectors) {
  bool hasRequirement = false;
  bool hasCallingSelector = false;
  bool hasCalledSelector = false;
  bool hasVersion = false;
  std::size_t offset = 0;
  while (offset < parameters.size()) {
    const auto parameter = parameters[offset++];
    std::size_t parameterLength = 0;
    if (!DecodeSessionLength(parameters, &offset, &parameterLength) ||
        parameterLength > parameters.size() - offset) {
      return Invalid("会话连接项参数长度无效");
    }
    const auto value = parameters.subspan(offset, parameterLength);
    offset += parameterLength;
    if (parameter == kSessionRequirementParameter) {
      hasRequirement = true;
      // 会话要求是连接项的嵌套结构，版本号位于其内部。
      std::size_t inner = 0;
      while (inner < value.size()) {
        const auto item = value[inner++];
        std::size_t itemLength = 0;
        if (!DecodeSessionLength(value, &inner, &itemLength) ||
            itemLength > value.size() - inner) {
          return Invalid("会话要求子项长度无效");
        }
        if (item == kVersionNumberParameter) {
          if (itemLength != 1 || value[inner] != kSessionVersionNumber) {
            return Invalid("会话版本号不是02");
          }
          hasVersion = true;
        }
        inner += itemLength;
      }
    } else if (parameter == kCallingSessionSelectorParameter) {
      hasCallingSelector = true;
    } else if (parameter == kCalledSessionSelectorParameter) {
      hasCalledSelector = true;
    }
  }
  if (!hasRequirement || !hasVersion) {
    return Invalid("会话连接项缺少会话要求或版本号");
  }
  if (requireSessionSelectors && (!hasCallingSelector || !hasCalledSelector)) {
    return Invalid("会话连接项缺少调用或被调会话选择子");
  }
  return grpc::Status::OK;
}

grpc::Status EncodeSessionWithUserData(
    IsoSessionPduType type, std::span<const std::uint8_t> userData,
    const IsoSessionSelectors& selectors, std::span<std::uint8_t> output,
    std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS ISO输出长度参数为空");
  }
  *outputSize = 0;
  if (userData.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS ISO用户数据不能为空");
  }
  std::array<std::uint8_t, 32> connectParameters{};
  std::size_t connectSize = 0;
  const auto hasConnectParameters = type == IsoSessionPduType::CONNECT ||
                                    type == IsoSessionPduType::ACCEPT;
  if (hasConnectParameters &&
      !BuildConnectParameters(selectors, &connectParameters, &connectSize)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS会话选择子长度无效");
  }
  const auto fixedSize = hasConnectParameters ? connectSize : 0;
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
      !AppendBytes(output, &offset,
                   std::span<const std::uint8_t>(connectParameters.data(),
                                                 connectSize))) {
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

// 追加一段已经按TLV组装好的字节。
void AppendEncoded(std::vector<std::uint8_t>* target,
                   const std::vector<std::uint8_t>& encoded) {
  if (target == nullptr) {
    return;
  }
  target->insert(target->end(), encoded.begin(), encoded.end());
}

// 把tag/value编码成BER字段后追加到target，避免逐处重复"编码+追加"。
grpc::Status AppendBerTlv(std::uint8_t tag,
                          std::span<const std::uint8_t> value,
                          std::vector<std::uint8_t>* target,
                          std::string_view reason) {
  std::vector<std::uint8_t> encoded;
  const auto status = EncodeBerTlv(tag, value, &encoded);
  if (!status.ok()) {
    return status;
  }
  if (target == nullptr) {
    return OutputError(reason);
  }
  AppendEncoded(target, encoded);
  return grpc::Status::OK;
}

// 追加一个BER整数字段。
grpc::Status AppendBerTlvInteger(std::uint8_t tag, std::uint32_t value,
                                 std::vector<std::uint8_t>* target,
                                 std::string_view reason) {
  std::vector<std::uint8_t> encoded;
  const auto status = EncodeBerInteger(tag, value, &encoded);
  if (!status.ok()) {
    return status;
  }
  if (target == nullptr) {
    return OutputError(reason);
  }
  AppendEncoded(target, encoded);
  return grpc::Status::OK;
}

// 追加一个BER OID字段。
grpc::Status AppendBerTlvOid(std::uint8_t tag,
                             std::span<const std::uint32_t> oid,
                             std::vector<std::uint8_t>* target,
                             std::string_view reason) {
  std::vector<std::uint8_t> encoded;
  const auto status = EncodeOid(tag, oid, &encoded);
  if (!status.ok()) {
    return status;
  }
  if (target == nullptr) {
    return OutputError(reason);
  }
  AppendEncoded(target, encoded);
  return grpc::Status::OK;
}

// 编码表示层CP：mode-selector + normal-mode-parameters（P-SEL + 上下文定义列表）
// + user-data。缺少CP时，实测装置收到裸AARQ后会直接回ABORT。
grpc::Status EncodePresentationCpInternal(
    std::span<const std::uint8_t> userData,
    std::span<const std::uint8_t> callingPresentationSelector,
    std::span<const std::uint8_t> calledPresentationSelector,
    std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS表示层输出参数为空");
  }
  output->clear();
  if (userData.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS表示层用户数据不能为空");
  }
  if (callingPresentationSelector.empty() ||
      calledPresentationSelector.empty() ||
      callingPresentationSelector.size() > 32 ||
      calledPresentationSelector.size() > 32) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS表示层选择子长度无效");
  }
  std::vector<std::uint8_t> field;
  std::vector<std::uint8_t> content;
  // a0 03 80 01 01：mode-selector = normal-mode。
  const std::array<std::uint8_t, 1> normalMode{0x01};
  auto status =
      AppendBerTlv(0x80, normalMode, &field, "表示层mode-selector写入失败");
  if (status.ok()) {
    status = AppendBerTlv(kModeSelectorTag, field, &content,
                          "表示层mode-selector写入失败");
  }
  if (!status.ok()) {
    return status;
  }

  std::vector<std::uint8_t> normalModeContent;
  status = AppendBerTlv(kCallingPresentationSelectorTag,
                        callingPresentationSelector, &normalModeContent,
                        "表示层调用选择子写入失败");
  if (status.ok()) {
    status = AppendBerTlv(kCalledPresentationSelectorTag,
                          calledPresentationSelector, &normalModeContent,
                          "表示层被调选择子写入失败");
  }
  if (!status.ok()) {
    return status;
  }

  // 上下文定义列表：ACSE（上下文1）与MMS（上下文3），两侧都必须带名称。
  const std::array<std::uint32_t, 5> acseContext{2, 2, 1, 0, 1};
  const std::array<std::uint32_t, 5> acseTransferSyntax{2, 2, 1, 0, 0};
  const std::array<std::uint32_t, 5> mmsTransferSyntax{2, 2, 1, 0, 1};
  const std::array<std::pair<std::uint32_t, std::span<const std::uint32_t>>, 2>
      contexts{{{1, acseContext},
                {kMmsPresentationContextId, kMmsAbstractSyntaxOid}}};
  std::vector<std::uint8_t> definitions;
  for (const auto& definition : contexts) {
    std::vector<std::uint8_t> pdv;
    status = AppendBerTlvInteger(0x02, definition.first, &pdv,
                                 "表示层上下文编号写入失败");
    if (status.ok()) {
      status = AppendBerTlvOid(0x06, definition.second, &pdv,
                               "表示层抽象语法写入失败");
    }
    const auto& transferSyntax =
        definition.first == 1 ? acseTransferSyntax : mmsTransferSyntax;
    if (status.ok()) {
      status = AppendBerTlvOid(0x06, transferSyntax, &pdv,
                               "表示层传输语法写入失败");
    }
    if (status.ok()) {
      status = AppendBerTlv(0x30, pdv, &definitions, "表示层上下文定义写入失败");
    }
    if (!status.ok()) {
      return status;
    }
  }
  status = AppendBerTlv(kContextDefinitionListTag, definitions,
                        &normalModeContent, "表示层上下文定义列表写入失败");
  if (status.ok()) {
    status = AppendBerTlv(kNormalModeParametersTag, normalModeContent, &content,
                          "表示层normal-mode-parameters写入失败");
  }
  if (status.ok()) {
    status = AppendBerTlv(kPresentationDataPdu, userData, &content,
                          "表示层user-data写入失败");
  }
  if (!status.ok()) {
    return status;
  }
  return EncodeBerTlv(0x31, content, output);
}

// 解析normal-mode-parameters：取出两个P-SEL，并校验上下文定义列表中的MMS
// 上下文编号与抽象语法OID，避免把非MMS的CP当成MMS关联接受。
grpc::Status DecodeNormalModeParameters(std::span<const std::uint8_t> value,
                                        PresentationCpView* cp) {
  bool hasContextDefinitionList = false;
  std::size_t offset = 0;
  while (offset < value.size()) {
    BerTlvView field;
    auto status = ReadBerTlv(value, &offset, &field);
    if (!status.ok()) {
      return status;
    }
    switch (field.tag) {
      case kCallingPresentationSelectorTag:
      case kCalledPresentationSelectorTag: {
        const auto isCalling = field.tag == kCallingPresentationSelectorTag;
        auto* selector = isCalling ? &cp->callingPresentationSelector
                                   : &cp->calledPresentationSelector;
        auto* selectorSize = isCalling ? &cp->callingPresentationSelectorSize
                                       : &cp->calledPresentationSelectorSize;
        if (field.value.size() > selector->size()) {
          return Invalid(isCalling ? "CP调用表示选择子过长"
                                   : "CP被调表示选择子过长");
        }
        std::copy(field.value.begin(), field.value.end(), selector->begin());
        *selectorSize = field.value.size();
        break;
      }
      case kContextDefinitionListTag: {
        hasContextDefinitionList = true;
        std::size_t listOffset = 0;
        bool hasMmsContext = false;
        while (listOffset < field.value.size()) {
          BerTlvView definition;
          status = ReadBerTlv(field.value, &listOffset, &definition);
          if (!status.ok() || definition.tag != 0x30) {
            return Invalid("CP上下文定义列表结构无效");
          }
          std::size_t itemOffset = 0;
          BerTlvView id;
          status = ReadBerTlv(definition.value, &itemOffset, &id);
          if (!status.ok() || id.tag != 0x02) {
            return Invalid("CP上下文定义缺少编号");
          }
          std::uint64_t contextId = 0;
          status = ReadBerUnsigned(id.value, &contextId);
          if (!status.ok()) {
            return status;
          }
          BerTlvView syntax;
          status = ReadBerTlv(definition.value, &itemOffset, &syntax);
          if (!status.ok() || syntax.tag != 0x06) {
            return Invalid("CP上下文定义缺少抽象语法");
          }
          if (contextId == kMmsPresentationContextId) {
            std::array<std::uint32_t, 16> arcs{};
            std::size_t arcCount = 0;
            status = ReadBerOid(syntax.value, arcs, &arcCount);
            if (!status.ok() || arcCount != kMmsAbstractSyntaxOid.size() ||
                !std::equal(arcs.begin(), arcs.begin() + arcCount,
                            kMmsAbstractSyntaxOid.begin())) {
              return Invalid("CP的MMS上下文抽象语法不是MMS");
            }
            hasMmsContext = true;
          }
          // 传输语法名称由对端自行声明，这里只要求TLV结构完整。
          while (itemOffset < definition.value.size()) {
            BerTlvView transferSyntax;
            status = ReadBerTlv(definition.value, &itemOffset, &transferSyntax);
            if (!status.ok()) {
              return status;
            }
          }
        }
        if (!hasMmsContext) {
          return Invalid("CP上下文定义列表缺少MMS表示上下文");
        }
        break;
      }
      default:
        // 其它可选CP字段只要求TLV完整，不参与MMS关联协商。
        break;
    }
  }
  if (cp->callingPresentationSelectorSize == 0 ||
      cp->calledPresentationSelectorSize == 0) {
    return Invalid("CP缺少调用或被调表示选择子");
  }
  if (!hasContextDefinitionList) {
    return Invalid("CP缺少表示上下文定义列表");
  }
  return grpc::Status::OK;
}

grpc::Status DecodePresentationCpInternal(std::span<const std::uint8_t> input,
                                          PresentationCpView* cp) {
  if (cp == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS表示层输出参数为空");
  }
  *cp = {};
  std::size_t offset = 0;
  BerTlvView outer;
  auto status = ReadBerTlv(input, &offset, &outer);
  if (!status.ok() || offset != input.size() || outer.tag != 0x31) {
    return Invalid("表示层CP外层标签或长度无效");
  }
  offset = 0;
  BerTlvView modeSelector;
  status = ReadBerTlv(outer.value, &offset, &modeSelector);
  if (!status.ok() || modeSelector.tag != kModeSelectorTag) {
    return Invalid("表示层CP缺少mode-selector");
  }
  std::size_t modeOffset = 0;
  BerTlvView mode;
  status = ReadBerTlv(modeSelector.value, &modeOffset, &mode);
  if (!status.ok() || mode.tag != 0x80 || mode.value.size() != 1 ||
      mode.value[0] != 0x01 || modeOffset != modeSelector.value.size()) {
    return Invalid("表示层CP不是normal-mode");
  }
  BerTlvView normalMode;
  status = ReadBerTlv(outer.value, &offset, &normalMode);
  if (!status.ok() || normalMode.tag != kNormalModeParametersTag) {
    return Invalid("表示层CP缺少normal-mode-parameters");
  }
  status = DecodeNormalModeParameters(normalMode.value, cp);
  if (!status.ok()) {
    return status;
  }
  if (offset < outer.value.size()) {
    BerTlvView userData;
    status = ReadBerTlv(outer.value, &offset, &userData);
    if (!status.ok() || userData.tag != kPresentationDataPdu) {
      return Invalid("表示层CP的user-data结构无效");
    }
    cp->userData = userData.value;
  }
  if (offset != outer.value.size()) {
    return Invalid("表示层CP包含多余字段");
  }
  return grpc::Status::OK;
}

grpc::Status EncodeExternalUserInformation(
    std::span<const std::uint8_t> mmsPdu, std::vector<std::uint8_t>* output) {
  if (mmsPdu.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS user-information不能为空");
  }
  // EXTERNAL的第1个字段必须是indirect-reference=3（指向表示层上下文里已定义为
  // MMS的PDV）；写成抽象语法OID时实测装置会回Session ABORT。
  std::vector<std::uint8_t> indirect;
  auto status = EncodeBerInteger(kIndirectReferenceTag, kMmsIndirectReference,
                                 &indirect);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> single;
  status = EncodeBerTlv(kSingleAsn1TypeTag, mmsPdu, &single);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> externalContent;
  externalContent.reserve(indirect.size() + single.size());
  externalContent.insert(externalContent.end(), indirect.begin(),
                         indirect.end());
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
  if (!status.ok()) {
    return status;
  }
  if (directReference.tag == kIndirectReferenceTag) {
    // 实测装置与libiec61850都使用indirect-reference=3指向MMS上下文。
    std::uint64_t reference = 0;
    status = ReadBerUnsigned(directReference.value, &reference);
    if (!status.ok() || reference != kMmsIndirectReference) {
      return Invalid("EXTERNAL indirect-reference不是MMS上下文");
    }
  } else if (directReference.tag == 0x06) {
    // 兼容旧实现：仍接受抽象语法OID形式的EXTERNAL。
    std::array<std::uint32_t, 16> arcs{};
    std::size_t arcCount = 0;
    status = ReadBerOid(directReference.value, arcs, &arcCount);
    if (!status.ok() || arcCount != kMmsAbstractSyntaxOid.size() ||
        !std::equal(arcs.begin(), arcs.begin() + arcCount,
                    kMmsAbstractSyntaxOid.begin())) {
      return Invalid("EXTERNAL抽象语法OID不是MMS");
    }
  } else {
    return Invalid("EXTERNAL缺少indirect-reference或抽象语法OID");
  }
  BerTlvView single;
  status = ReadBerTlv(encoded.value, &offset, &single);
  if (!status.ok() || single.tag != kSingleAsn1TypeTag ||
      offset != encoded.value.size()) {
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
    std::span<std::uint8_t> output, std::size_t* outputSize,
    const IsoSessionSelectors& selectors) {
  return EncodeSessionWithUserData(IsoSessionPduType::CONNECT,
                                   presentationData, selectors, output,
                                   outputSize);
}

grpc::Status EncodeIsoSessionAccept(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize,
    const IsoSessionSelectors& selectors) {
  return EncodeSessionWithUserData(IsoSessionPduType::ACCEPT, presentationData,
                                   selectors, output, outputSize);
}

grpc::Status EncodeIsoSessionData(std::span<const std::uint8_t> presentationData,
                                  std::span<std::uint8_t> output,
                                  std::size_t* outputSize) {
  return EncodeSessionDataInternal(presentationData, output, outputSize);
}

grpc::Status EncodeIsoSessionFinish(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeSessionWithUserData(IsoSessionPduType::FINISH, presentationData,
                                   IsoSessionSelectors{}, output, outputSize);
}

grpc::Status EncodeIsoSessionDisconnect(
    std::span<const std::uint8_t> presentationData,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  return EncodeSessionWithUserData(IsoSessionPduType::DISCONNECT,
                                   presentationData, IsoSessionSelectors{},
                                   output, outputSize);
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
      pdu->type = type;
      if (pdu->userData.empty()) {
        return Invalid("SPDU缺少用户数据参数");
      }
      return grpc::Status::OK;
    case IsoSessionPduType::ACCEPT: {
      // 实测装置回的ACCEPT带完整连接项：会话版本必须为02；会话选择子按实际
      // 报文校验，缺失时只提示不拒绝，避免把不同实现的ACCEPT误判为失败。
      const auto status =
          ValidateConnectParameters(input.subspan(offset, totalLength), false);
      if (!status.ok()) {
        return status;
      }
      if (pdu->userData.empty()) {
        return Invalid("SPDU缺少用户数据参数");
      }
      pdu->type = type;
      return grpc::Status::OK;
    }
    case IsoSessionPduType::FINISH:
    case IsoSessionPduType::DISCONNECT:
      if (pdu->userData.empty()) {
        return Invalid("SPDU缺少用户数据参数");
      }
      pdu->type = type;
      return grpc::Status::OK;
    case IsoSessionPduType::REFUSE:
    case IsoSessionPduType::ABORT:
      // REFUSE表示对端在ACSE阶段拒绝了关联；原因字段由调用方按SPDU原文记录。
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

grpc::Status EncodeMmsAcsePdu(
    std::uint8_t applicationTag,
    std::span<const std::uint32_t> applicationContextOid, std::uint32_t result,
    bool includeResult, std::span<const std::uint8_t> mmsPdu,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS ACSE输出长度参数为空");
  }
  *outputSize = 0;
  std::vector<std::uint8_t> encoded;
  const auto status = EncodeAcsePdu(applicationTag, applicationContextOid,
                                    mmsPdu, result, includeResult, &encoded);
  if (!status.ok()) {
    return status;
  }
  if (encoded.size() > output.size()) {
    return OutputError("ACSE报文");
  }
  std::copy(encoded.begin(), encoded.end(), output.begin());
  *outputSize = encoded.size();
  return grpc::Status::OK;
}

grpc::Status EncodeMmsPresentationCp(
    std::span<const std::uint8_t> userData,
    std::span<const std::uint8_t> callingPresentationSelector,
    std::span<const std::uint8_t> calledPresentationSelector,
    std::span<std::uint8_t> output, std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS表示层CP输出长度参数为空");
  }
  *outputSize = 0;
  std::vector<std::uint8_t> encoded;
  const auto status = EncodePresentationCpInternal(
      userData, callingPresentationSelector, calledPresentationSelector,
      &encoded);
  if (!status.ok()) {
    return status;
  }
  if (encoded.size() > output.size()) {
    return OutputError("表示层CP报文");
  }
  std::copy(encoded.begin(), encoded.end(), output.begin());
  *outputSize = encoded.size();
  return grpc::Status::OK;
}

grpc::Status DecodeMmsPresentationCp(std::span<const std::uint8_t> input,
                                     PresentationCpView* cp) {
  return DecodePresentationCpInternal(input, cp);
}

grpc::Status DecodeMmsAcsePdu(std::span<const std::uint8_t> input,
                              std::uint8_t expectedTag, MmsAareView* result) {
  return DecodeAcsePdu(input, expectedTag, result);
}

// 服务端/模拟器侧的解封装：带表示层CP的客户端先取出CP的user-data，
// 旧的不带CP的客户端直接按裸ACSE解析，两种格式都必须支持。
grpc::Status DecodeDelegatedAcsePdu(std::span<const std::uint8_t> input,
                                    std::uint8_t expectedTag,
                                    MmsAareView* result) {
  if (result == nullptr || input.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS ACSE输入参数为空");
  }
  if (input.front() == 0x31) {
    PresentationCpView cp;
    const auto cpStatus = DecodePresentationCpInternal(input, &cp);
    if (!cpStatus.ok()) {
      return cpStatus;
    }
    if (cp.userData.empty()) {
      return grpc::Status(grpc::StatusCode::DATA_LOSS,
                          "IEC61850 MMS表示层CP缺少用户数据");
    }
    return DecodeAcsePdu(cp.userData, expectedTag, result);
  }
  return DecodeAcsePdu(input, expectedTag, result);
}

grpc::Status DecodeMmsAare(std::span<const std::uint8_t> input,
                           MmsAareView* result) {
  return DecodeDelegatedAcsePdu(input, 0x61, result);
}

grpc::Status DecodeMmsAarq(std::span<const std::uint8_t> input,
                           MmsAareView* result) {
  return DecodeDelegatedAcsePdu(input, 0x60, result);
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
  auto status = EncodeBerTlv(kSingleAsn1TypeTag, mmsPdu, &single);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> contextId;
  status = EncodeBerInteger(0x02, kMmsPresentationContextId, &contextId);
  if (!status.ok()) {
    return status;
  }
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
  if (!status.ok() || context != kMmsPresentationContextId) {
    return Invalid("P-DATA-TF不是MMS Presentation Context");
  }
  BerTlvView single;
  status = ReadBerTlv(sequence.value, &offset, &single);
  if (!status.ok() || offset != sequence.value.size() ||
      single.tag != kSingleAsn1TypeTag) {
    return Invalid("P-DATA-TF缺少single-ASN1-type");
  }
  *mmsPdu = single.value;
  return grpc::Status::OK;
}

}  // namespace IEC61850
