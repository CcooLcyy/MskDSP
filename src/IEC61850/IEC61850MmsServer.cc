#include "IEC61850MmsServer.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include "IEC61850MmsBer.h"

namespace IEC61850 {
namespace {

grpc::Status Invalid(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      std::string("IEC61850 MMS服务端参数无效: ") +
                          std::string(reason));
}

grpc::Status Unsupported(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                      std::string("IEC61850 MMS服务端未实现: ") +
                          std::string(reason));
}

grpc::Status Tlv(std::uint32_t tag, std::span<const std::uint8_t> value,
                 std::vector<std::uint8_t>* output) {
  if (output == nullptr) return Invalid("BER输出为空");
  output->assign(65536, 0);
  BerWriter writer(*output);
  if (!writer.Tlv(tag, value)) return grpc::Status(
      grpc::StatusCode::RESOURCE_EXHAUSTED, "IEC61850 MMS服务端BER响应超过上限");
  output->resize(writer.size());
  return grpc::Status::OK;
}

grpc::Status EncodeIdentifier(std::string_view value,
                              std::vector<std::uint8_t>* output) {
  return Tlv(0x1a,
             std::span<const std::uint8_t>(
                 reinterpret_cast<const std::uint8_t*>(value.data()),
                 value.size()),
             output);
}

std::string ObjectKey(const MmsObjectName& object) {
  return object.domain + "\x1f" + object.identifier;
}

std::string DisplayListName(std::string_view key) {
  const auto separator = key.find('\x1f');
  return separator == std::string_view::npos
             ? std::string(key)
             : std::string(key.substr(separator + 1));
}

}  // namespace

MmsServer::MmsServer(MmsServerModel model) : model_(std::move(model)) {}

void MmsServer::SetModel(MmsServerModel model) {
  std::lock_guard lock(mutex_);
  model_ = std::move(model);
}

MmsBitString MmsServer::SupportedServicesLocked() const {
  if (model_.serviceSupport.size != 0) {
    return model_.serviceSupport;
  }
  MmsBitString support;
  support.size = 11;
  support.unusedBits = 3;
  // GetNameList和动态Named Variable List属性由本服务端模型直接处理。
  support.bytes[0] = 0x40;
  support.bytes[1] = 0x1c;  // Define、GetAttributes、Delete NVL。
  if (model_.read) {
    support.bytes[0] |= 0x08;
  }
  if (model_.write) {
    support.bytes[0] |= 0x04;
  }
  if (model_.fileDirectory) {
    support.bytes[9] |= 0x04;
  }
  if (model_.journalRead) {
    // ConfirmedService=65位于支持位串第八字节的次高位。
    support.bytes[8] |= 0x40;
  }
  return support;
}

grpc::Status MmsServer::HandleInitiate(
    std::span<const std::uint8_t> request,
    std::vector<std::uint8_t>* response) {
  if (response == nullptr) return Invalid("Initiate响应输出为空");
  response->clear();
  MmsInitiateRequest initiate;
  auto status = DecodeMmsInitiateRequest(request, &initiate);
  if (!status.ok()) return status;
  if (initiate.proposedServiceSupport.size != 11 ||
      initiate.proposedServiceSupport.unusedBits != 3 ||
      initiate.proposedParameterSupport.size != 2 ||
      initiate.proposedParameterSupport.unusedBits != 5) {
    return Invalid("Initiate支持位串长度不符合当前MMS边界");
  }

  MmsInitiateResponse negotiated;
  negotiated.hasLocalDetailCalled = false;
  negotiated.negotiatedMaxServOutstandingCalling =
      initiate.hasProposedMaxServOutstandingCalling
          ? initiate.proposedMaxServOutstandingCalling
          : 10;
  negotiated.negotiatedMaxServOutstandingCalled =
      initiate.hasProposedMaxServOutstandingCalled
          ? initiate.proposedMaxServOutstandingCalled
          : 10;
  negotiated.negotiatedDataStructureNestingLevel =
      initiate.hasProposedDataStructureNestingLevel
          ? initiate.proposedDataStructureNestingLevel
          : 32;
  negotiated.negotiatedVersionNumber =
      std::min<std::uint8_t>(initiate.proposedVersionNumber, 1);
  negotiated.negotiatedParameterSupport.size = 2;
  negotiated.negotiatedParameterSupport.unusedBits = 5;
  MmsBitString supported;
  MmsBitString supportedParameters;
  {
    std::lock_guard lock(mutex_);
    supported = SupportedServicesLocked();
    supportedParameters = model_.parameterSupport;
  }
  if (supported.size != 11 || supported.unusedBits != 3) {
    return Invalid("服务端服务支持位串长度无效");
  }
  if (supportedParameters.size == 0) {
    supportedParameters.size = 2;
    supportedParameters.unusedBits = 5;
  }
  if (supportedParameters.size != 2 || supportedParameters.unusedBits != 5) {
    return Invalid("服务端参数支持位串长度无效");
  }
  negotiated.negotiatedParameterSupport = supportedParameters;
  for (std::size_t index = 0; index < supportedParameters.size; ++index) {
    negotiated.negotiatedParameterSupport.bytes[index] =
        static_cast<std::uint8_t>(
            supportedParameters.bytes[index] &
            initiate.proposedParameterSupport.bytes[index]);
  }
  negotiated.negotiatedServiceSupport = supported;
  for (std::size_t index = 0; index < supported.size; ++index) {
    negotiated.negotiatedServiceSupport.bytes[index] =
        static_cast<std::uint8_t>(supported.bytes[index] &
                                  initiate.proposedServiceSupport.bytes[index]);
  }
  std::array<std::uint8_t, 4096> encoded{};
  std::size_t size = 0;
  status = EncodeMmsInitiateResponse(negotiated, encoded, &size);
  if (!status.ok()) return status;
  response->assign(encoded.begin(), encoded.begin() + size);
  return grpc::Status::OK;
}

grpc::Status MmsServer::HandleConfirmed(
    std::span<const std::uint8_t> request, std::vector<std::uint8_t>* response) {
  if (response == nullptr) return Invalid("响应输出为空");
  response->clear();
  MmsConfirmedPduView pdu;
  auto status = DecodeMmsConfirmedRequest(request, &pdu);
  if (!status.ok()) return status;
  switch (pdu.serviceTag) {
    case 1:
      return HandleNameList(pdu, request, response);
    case 4:
      return HandleRead(pdu, request, response);
    case 5:
      return HandleWrite(pdu, request, response);
    case 11:
    case 12:
    case 13:
      return HandleNamedVariableList(pdu, request, response);
    case 65: {
      MmsJournalReadRequest journalRequest;
      std::uint32_t invokeId = 0;
      status = DecodeMmsJournalReadRequest(request, &invokeId, &journalRequest);
      if (!status.ok()) return status;
      MmsJournalReadResponse journalResponse;
      std::function<grpc::Status(const MmsJournalReadRequest*,
                                  MmsJournalReadResponse*)>
          handler;
      {
        std::lock_guard lock(mutex_);
        handler = model_.journalRead;
      }
      if (handler) {
        status = handler(&journalRequest, &journalResponse);
        if (!status.ok()) return status;
      }
      std::array<std::uint8_t, 65536> encoded{};
      std::size_t size = 0;
      status = EncodeMmsJournalReadResponse(invokeId, journalResponse, encoded,
                                            &size);
      if (!status.ok()) return status;
      response->assign(encoded.begin(), encoded.begin() + size);
      return grpc::Status::OK;
    }
    case 77: {
      MmsFileDirectoryRequest directoryRequest;
      std::uint32_t invokeId = 0;
      status = DecodeMmsFileDirectoryRequest(request, &invokeId,
                                             &directoryRequest);
      if (!status.ok()) return status;
      MmsFileDirectoryResponse directoryResponse;
      std::function<grpc::Status(const MmsFileDirectoryRequest*,
                                  MmsFileDirectoryResponse*)>
          handler;
      {
        std::lock_guard lock(mutex_);
        handler = model_.fileDirectory;
      }
      if (handler) {
        status = handler(&directoryRequest, &directoryResponse);
        if (!status.ok()) return status;
      }
      std::array<std::uint8_t, 65536> encoded{};
      std::size_t size = 0;
      status = EncodeMmsFileDirectoryResponse(invokeId, directoryResponse,
                                               encoded, &size);
      if (!status.ok()) return status;
      response->assign(encoded.begin(), encoded.begin() + size);
      return grpc::Status::OK;
    }
    default:
      return Unsupported("当前Confirmed服务选择");
  }
}

grpc::Status MmsServer::HandleNameList(
    const MmsConfirmedPduView& pdu, std::span<const std::uint8_t> request,
    std::vector<std::uint8_t>* response) {
  MmsGetNameListRequest nameList;
  std::uint32_t invokeId = 0;
  auto status = DecodeMmsGetNameListRequest(request, &invokeId, &nameList);
  if (!status.ok()) return status;
  std::vector<std::string> identifiers;
  {
    std::lock_guard lock(mutex_);
    if (nameList.objectClass == MmsObjectClass::DOMAIN) {
      identifiers = model_.domains;
    } else if (nameList.objectClass == MmsObjectClass::NAMED_VARIABLE) {
      identifiers = model_.namedVariables;
    } else if (nameList.objectClass == MmsObjectClass::NAMED_VARIABLE_LIST) {
      identifiers.reserve(model_.namedVariableLists.size());
      for (const auto& [key, _] : model_.namedVariableLists) {
        identifiers.push_back(DisplayListName(key));
      }
      std::sort(identifiers.begin(), identifiers.end());
    } else {
      return EncodeNameListResponse(invokeId, {}, false, response);
    }
  }
  auto begin = nameList.continueAfter.has_value()
                   ? std::find(identifiers.begin(), identifiers.end(),
                               *nameList.continueAfter)
                   : identifiers.begin();
  if (begin == identifiers.end() && nameList.continueAfter.has_value()) {
    return EncodeNameListResponse(invokeId, {}, false, response);
  }
  if (nameList.continueAfter.has_value()) {
    ++begin;
  }
  const std::size_t pageSize = std::min<std::size_t>(256, identifiers.end() - begin);
  const bool more = begin + pageSize != identifiers.end();
  return EncodeNameListResponse(
      invokeId, {begin, begin + pageSize}, more, response);
}

grpc::Status MmsServer::HandleRead(
    const MmsConfirmedPduView&, std::span<const std::uint8_t> request,
    std::vector<std::uint8_t>* response) {
  MmsReadRequest readRequest;
  std::uint32_t invokeId = 0;
  auto status = DecodeMmsReadRequest(request, &invokeId, &readRequest);
  if (!status.ok()) return status;
  std::function<grpc::Status(const MmsReadRequest&, MmsReadResponse*)> handler;
  {
    std::lock_guard lock(mutex_);
    handler = model_.read;
  }
  if (!handler) return Unsupported("Read数据模型回调");
  MmsReadResponse readResponse;
  status = handler(readRequest, &readResponse);
  if (!status.ok()) return status;
  std::array<std::uint8_t, 65536> encoded{};
  std::size_t size = 0;
  status = EncodeMmsReadResponse(invokeId, readResponse, encoded, &size);
  if (!status.ok()) return status;
  response->assign(encoded.begin(), encoded.begin() + size);
  return grpc::Status::OK;
}

grpc::Status MmsServer::HandleWrite(
    const MmsConfirmedPduView&, std::span<const std::uint8_t> request,
    std::vector<std::uint8_t>* response) {
  MmsWriteRequest writeRequest;
  std::uint32_t invokeId = 0;
  auto status = DecodeMmsWriteRequest(request, &invokeId, &writeRequest);
  if (!status.ok()) return status;
  std::function<grpc::Status(const MmsWriteRequest&, MmsWriteResponse*)> handler;
  {
    std::lock_guard lock(mutex_);
    handler = model_.write;
  }
  if (!handler) return Unsupported("Write数据模型回调");
  MmsWriteResponse writeResponse;
  status = handler(writeRequest, &writeResponse);
  if (!status.ok()) return status;
  std::array<std::uint8_t, 65536> encoded{};
  std::size_t size = 0;
  status = EncodeMmsWriteResponse(invokeId, writeResponse, encoded, &size);
  if (!status.ok()) return status;
  response->assign(encoded.begin(), encoded.begin() + size);
  return grpc::Status::OK;
}

grpc::Status MmsServer::HandleNamedVariableList(
    const MmsConfirmedPduView& pdu, std::span<const std::uint8_t> request,
    std::vector<std::uint8_t>* response) {
  std::uint32_t invokeId = 0;
  MmsObjectName listName;
  auto status = grpc::Status::OK;
  if (pdu.serviceTag == 11) {
    MmsNamedVariableListDefinition definition;
    status = DecodeMmsDefineNamedVariableListRequest(request, &invokeId,
                                                     &definition);
    if (!status.ok()) return status;
    {
      std::lock_guard lock(mutex_);
      model_.namedVariableLists[ObjectKey(definition.listName)] =
          definition.variables;
    }
    std::array<std::uint8_t, 256> encoded{};
    std::size_t size = 0;
    status = EncodeMmsConfirmedResponse(invokeId, 11, {}, encoded, &size);
    if (status.ok()) response->assign(encoded.begin(), encoded.begin() + size);
    return status;
  }
  if (pdu.serviceTag == 13) {
    status = DecodeMmsDeleteNamedVariableListRequest(request, &invokeId,
                                                     &listName);
    if (!status.ok()) return status;
  } else {
    status = DecodeMmsObjectName(pdu.serviceValue, &listName);
    invokeId = pdu.invokeId;
    if (!status.ok()) return status;
  }
  if (pdu.serviceTag == 13) {
    std::lock_guard lock(mutex_);
    model_.namedVariableLists.erase(ObjectKey(listName));
    std::array<std::uint8_t, 256> encoded{};
    std::size_t size = 0;
    status = EncodeMmsConfirmedResponse(invokeId, 13, {}, encoded, &size);
    if (status.ok()) response->assign(encoded.begin(), encoded.begin() + size);
    return status;
  }
  std::vector<MmsObjectName> variables;
  {
    std::lock_guard lock(mutex_);
    const auto it = model_.namedVariableLists.find(ObjectKey(listName));
    if (it == model_.namedVariableLists.end()) return Invalid("Named Variable List不存在");
    variables = it->second;
  }
  return EncodeNamedVariableListAttributesResponse(invokeId, variables, response);
}

grpc::Status MmsServer::EncodeNameListResponse(
    std::uint32_t invokeId, const std::vector<std::string>& identifiers,
    bool moreFollows, std::vector<std::uint8_t>* response) const {
  std::vector<std::uint8_t> listValue;
  for (const auto& identifier : identifiers) {
    std::vector<std::uint8_t> encoded;
    auto status = EncodeIdentifier(identifier, &encoded);
    if (!status.ok()) return status;
    listValue.insert(listValue.end(), encoded.begin(), encoded.end());
  }
  std::vector<std::uint8_t> list;
  auto status = Tlv(0xa0, listValue, &list);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> content = list;
  std::array<std::uint8_t, 8> boolValue{};
  BerWriter writer(boolValue);
  if (!writer.Boolean(0x81, moreFollows)) return Invalid("moreFollows编码失败");
  content.insert(content.end(), boolValue.begin(), boolValue.begin() + writer.size());
  std::array<std::uint8_t, 65536> encoded{};
  std::size_t size = 0;
  status = EncodeMmsConfirmedResponse(invokeId, 1, content, encoded, &size);
  if (status.ok()) response->assign(encoded.begin(), encoded.begin() + size);
  return status;
}

grpc::Status MmsServer::EncodeNamedVariableListAttributesResponse(
    std::uint32_t invokeId, const std::vector<MmsObjectName>& variables,
    std::vector<std::uint8_t>* response) const {
  std::vector<std::uint8_t> listValue;
  for (const auto& variable : variables) {
    std::vector<std::uint8_t> object;
    auto status = EncodeMmsObjectName(variable, &object);
    if (!status.ok()) return status;
    std::vector<std::uint8_t> specification;
    specification.assign(65536, 0);
    BerWriter specificationWriter(specification);
    if (!specificationWriter.Tlv(0xa0, object)) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "Named Variable List成员编码超过上限");
    }
    specification.resize(specificationWriter.size());
    std::vector<std::uint8_t> item;
    item.assign(65536, 0);
    BerWriter itemWriter(item);
    if (!itemWriter.Tlv(0x30, specification)) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "Named Variable List成员结构编码超过上限");
    }
    item.resize(itemWriter.size());
    listValue.insert(listValue.end(), item.begin(), item.end());
  }
  std::vector<std::uint8_t> deletable;
  deletable.assign(8, 0);
  BerWriter deletableWriter(deletable);
  if (!deletableWriter.Boolean(0x80, false)) {
    return Invalid("mmsDeletable编码失败");
  }
  deletable.resize(deletableWriter.size());
  std::vector<std::uint8_t> list;
  list.assign(65536, 0);
  BerWriter listWriter(list);
  if (!listWriter.Tlv(0xa1, listValue)) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "Named Variable List成员列表编码超过上限");
  }
  list.resize(listWriter.size());
  deletable.insert(deletable.end(), list.begin(), list.end());
  std::array<std::uint8_t, 65536> encoded{};
  std::size_t size = 0;
  auto status = EncodeMmsConfirmedResponse(invokeId, 12, deletable, encoded,
                                           &size);
  if (status.ok()) response->assign(encoded.begin(), encoded.begin() + size);
  return status;
}

}  // namespace IEC61850
