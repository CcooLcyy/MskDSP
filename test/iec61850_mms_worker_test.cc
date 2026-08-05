#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "IEC61850MmsPdu.h"
#include "IEC61850MmsBer.h"
#include "IEC61850MmsService.h"
#include "IEC61850MmsIsoSession.h"
#include "IEC61850MmsWorker.h"

namespace {

using namespace std::chrono_literals;

struct FactoryState {
  std::mutex mutex;
  std::condition_variable condition;
  std::size_t factoryCalls = 0;
  std::size_t connectCalls = 0;
  std::size_t connectCallsA = 0;
  std::size_t connectCallsB = 0;
  std::size_t sendCalls = 0;
  std::size_t sendCallsA = 0;
  std::size_t sendCallsB = 0;
  std::size_t closeCalls = 0;
  std::vector<std::uint32_t> connectTimeouts;
  std::vector<std::uint32_t> sendTimeouts;
  std::vector<std::uint32_t> receiveTimeouts;
  std::vector<std::size_t> sessionSendCounts;
  std::vector<IEC61850Proto::NetworkChannel> sessionChannels;
  std::vector<IEC61850Proto::NetworkChannel> channels;
  std::vector<IEC61850::MmsTransportEndpoint> endpoints;
  std::chrono::milliseconds connectDelay{};
};

// 控制脚本传输的单次发送闸门，用于稳定制造队列中请求等待超时的场景。
struct SendGate {
  std::mutex mutex;
  std::condition_variable condition;
  bool enabled = false;
  bool entered = false;
  bool released = false;
};

void AppendTlv(std::vector<std::uint8_t>* output, std::uint8_t tag,
               std::span<const std::uint8_t> value) {
  if (output == nullptr || value.size() > 0xffffu) {
    return;
  }
  output->push_back(tag);
  if (value.size() <= 127u) {
    output->push_back(static_cast<std::uint8_t>(value.size()));
  } else if (value.size() <= 0xffu) {
    output->push_back(0x81);
    output->push_back(static_cast<std::uint8_t>(value.size()));
  } else {
    output->push_back(0x82);
    output->push_back(static_cast<std::uint8_t>(value.size() >> 8));
    output->push_back(static_cast<std::uint8_t>(value.size()));
  }
  output->insert(output->end(), value.begin(), value.end());
}

void AppendTlv(std::vector<std::uint8_t>* output, std::uint8_t tag,
               const std::vector<std::uint8_t>& value) {
  AppendTlv(output, tag,
            std::span<const std::uint8_t>(value.data(), value.size()));
}

void AppendEncoded(std::vector<std::uint8_t>* output,
                   const std::vector<std::uint8_t>& encoded) {
  output->insert(output->end(), encoded.begin(), encoded.end());
}

template <std::size_t Size>
void AppendTlv(std::vector<std::uint8_t>* output, std::uint8_t tag,
               const std::array<std::uint8_t, Size>& value) {
  AppendTlv(output, tag, std::span<const std::uint8_t>(value));
}

std::vector<std::uint8_t> MakeSessionAccept(bool includeWrite = false) {
  IEC61850::MmsInitiateResponse response;
  response.negotiatedParameterSupport.size = 2;
  response.negotiatedParameterSupport.unusedBits = 5;
  response.negotiatedServiceSupport.size = 11;
  response.negotiatedServiceSupport.unusedBits = 3;
  response.negotiatedServiceSupport.bytes[0] =
      static_cast<std::uint8_t>(0x4a | (includeWrite ? 0x04 : 0));
  response.negotiatedServiceSupport.bytes[1] = 0x08;

  std::array<std::uint8_t, 4096> initiateBuffer{};
  std::size_t initiateSize = 0;
  if (!IEC61850::EncodeMmsInitiateResponse(response, initiateBuffer,
                                           &initiateSize)
           .ok()) {
    return {};
  }

  std::array<std::uint8_t, 4096> aareBuffer{};
  std::size_t aareSize = 0;
  if (!IEC61850::EncodeMmsAare(
           IEC61850::kMmsApplicationContextOid, 0,
           std::span<const std::uint8_t>(initiateBuffer.data(), initiateSize),
           aareBuffer, &aareSize)
           .ok()) {
    return {};
  }

  std::array<std::uint8_t, 4096> sessionBuffer{};
  std::size_t sessionSize = 0;
  if (!IEC61850::EncodeIsoSessionAccept(
           std::span<const std::uint8_t>(aareBuffer.data(), aareSize),
           sessionBuffer, &sessionSize)
           .ok()) {
    return {};
  }
  return std::vector<std::uint8_t>(sessionBuffer.begin(),
                                  sessionBuffer.begin() + sessionSize);
}

std::vector<std::uint8_t> MakeEmptyNameListResponse(std::uint32_t invokeId = 1) {
  const std::vector<std::uint8_t> identifiers;
  std::vector<std::uint8_t> service;
  AppendTlv(&service, 0xa0,
            std::span<const std::uint8_t>(identifiers.data(),
                                          identifiers.size()));
  const std::array<std::uint8_t, 1> noMore{0x00};
  AppendTlv(&service, 0x81, noMore);

  std::array<std::uint8_t, 4096> mmsBuffer{};
  std::size_t mmsSize = 0;
  if (!IEC61850::EncodeMmsConfirmedResponse(invokeId, 1, service, mmsBuffer,
                                            &mmsSize)
           .ok()) {
    return {};
  }

  std::array<std::uint8_t, 4096> presentationBuffer{};
  std::size_t presentationSize = 0;
  if (!IEC61850::EncodeMmsPresentationData(
           std::span<const std::uint8_t>(mmsBuffer.data(), mmsSize),
           presentationBuffer, &presentationSize)
           .ok()) {
    return {};
  }

  std::array<std::uint8_t, 4096> sessionBuffer{};
  std::size_t sessionSize = 0;
  if (!IEC61850::EncodeIsoSessionData(
           std::span<const std::uint8_t>(presentationBuffer.data(),
                                         presentationSize),
           sessionBuffer, &sessionSize)
           .ok()) {
    return {};
  }
  return std::vector<std::uint8_t>(sessionBuffer.begin(),
                                  sessionBuffer.begin() + sessionSize);
}

std::vector<std::uint8_t> WrapMmsResponse(
    std::uint32_t invokeId, std::uint8_t serviceTag,
    std::span<const std::uint8_t> service) {
  std::array<std::uint8_t, 4096> mmsBuffer{};
  std::size_t mmsSize = 0;
  if (!IEC61850::EncodeMmsConfirmedResponse(invokeId, serviceTag, service,
                                            mmsBuffer, &mmsSize)
           .ok()) {
    return {};
  }

  std::array<std::uint8_t, 4096> presentationBuffer{};
  std::size_t presentationSize = 0;
  if (!IEC61850::EncodeMmsPresentationData(
           std::span<const std::uint8_t>(mmsBuffer.data(), mmsSize),
           presentationBuffer, &presentationSize)
           .ok()) {
    return {};
  }

  std::array<std::uint8_t, 4096> sessionBuffer{};
  std::size_t sessionSize = 0;
  if (!IEC61850::EncodeIsoSessionData(
           std::span<const std::uint8_t>(presentationBuffer.data(),
                                         presentationSize),
           sessionBuffer, &sessionSize)
           .ok()) {
    return {};
  }
  return std::vector<std::uint8_t>(sessionBuffer.begin(),
                                  sessionBuffer.begin() + sessionSize);
}

std::vector<std::uint8_t> MakeNameListResponse(
    std::uint32_t invokeId, const std::vector<std::string>& identifiers) {
  std::vector<std::uint8_t> list;
  for (const auto& identifier : identifiers) {
    AppendTlv(
        &list, 0x1a,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(identifier.data()),
            identifier.size()));
  }
  std::vector<std::uint8_t> service;
  AppendTlv(&service, 0xa0, list);
  const std::array<std::uint8_t, 1> noMore{0x00};
  AppendTlv(&service, 0x81, noMore);
  return WrapMmsResponse(invokeId, 1, service);
}

std::vector<std::uint8_t> MakeBooleanAttributesResponse(
    std::uint32_t invokeId) {
  const std::array<std::uint8_t, 1> falseValue{0x00};
  std::vector<std::uint8_t> service;
  AppendTlv(&service, 0x80, falseValue);
  const std::vector<std::uint8_t> booleanType{0x83, 0x00};
  AppendTlv(&service, 0xa2, booleanType);
  return WrapMmsResponse(invokeId, 6, service);
}

std::vector<std::uint8_t> MakeTypedAttributesResponse(
    std::uint32_t invokeId, IEC61850::MmsTypeSpecificationKind kind,
    std::uint32_t width = 32) {
  std::vector<std::uint8_t> type;
  switch (kind) {
    case IEC61850::MmsTypeSpecificationKind::BOOLEAN: {
      // BOOLEAN TypeSpecification是空值选择，不能携带BOOLEAN Data值。
      AppendTlv(&type, 0x83, std::span<const std::uint8_t>{});
      break;
    }
    case IEC61850::MmsTypeSpecificationKind::INTEGER: {
      const std::array<std::uint8_t, 1> value{
          static_cast<std::uint8_t>(width)};
      AppendTlv(&type, 0x85, value);
      break;
    }
    case IEC61850::MmsTypeSpecificationKind::UNSIGNED: {
      const std::array<std::uint8_t, 1> value{
          static_cast<std::uint8_t>(width)};
      AppendTlv(&type, 0x86, value);
      break;
    }
    default:
      return {};
  }
  const std::array<std::uint8_t, 1> falseValue{0x00};
  std::vector<std::uint8_t> service;
  AppendTlv(&service, 0x80, falseValue);
  AppendTlv(&service, 0xa2, type);
  return WrapMmsResponse(invokeId, 6, service);
}

bool DecodeRequestedVariable(const IEC61850::MmsConfirmedPduView& request,
                             IEC61850::MmsObjectName* object) {
  if (object == nullptr) {
    return false;
  }
  std::size_t offset = 0;
  IEC61850::BerTlvView variable;
  if (!IEC61850::ReadBerTlv(request.serviceValue, &offset, &variable).ok() ||
      offset != request.serviceValue.size() || variable.tag != 0xa0) {
    return false;
  }
  return IEC61850::DecodeMmsObjectName(variable.value, object).ok();
}

std::vector<std::uint8_t> MakeNamedVariableListResponse(
    std::uint32_t invokeId, std::string_view domain,
    std::string_view identifier) {
  const std::array<std::uint8_t, 1> falseValue{0x00};
  std::vector<std::uint8_t> service;
  AppendTlv(&service, 0x80, falseValue);

  std::vector<std::uint8_t> domainObject;
  AppendTlv(&domainObject, 0x1a,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(domain.data()),
                domain.size()));
  AppendTlv(&domainObject, 0x1a,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(identifier.data()),
                identifier.size()));
  std::vector<std::uint8_t> objectName;
  AppendTlv(&objectName, 0xa1, domainObject);
  std::vector<std::uint8_t> specification;
  AppendTlv(&specification, 0xa0, objectName);
  std::vector<std::uint8_t> variable;
  AppendTlv(&variable, 0x30, specification);
  std::vector<std::uint8_t> list;
  list.insert(list.end(), variable.begin(), variable.end());
  AppendTlv(&service, 0xa1, list);
  return WrapMmsResponse(invokeId, 12, service);
}

using ScriptedResponses = std::vector<std::vector<std::uint8_t>>;

ScriptedResponses MakeDirectoryResponse(
    std::span<const std::uint8_t> payload, std::size_t /*sendCount*/) {
  IEC61850::IsoSessionPduView sessionPdu;
  if (!IEC61850::DecodeIsoSessionPdu(payload, &sessionPdu).ok() ||
      sessionPdu.type != IEC61850::IsoSessionPduType::DATA) {
    return {};
  }
  std::span<const std::uint8_t> mmsPdu;
  if (!IEC61850::DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu)
           .ok()) {
    return {};
  }
  IEC61850::MmsConfirmedPduView request;
  if (!IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok()) {
    return {};
  }
  if (request.serviceTag == 1) {
    IEC61850::MmsGetNameListRequest decoded;
    std::uint32_t invokeId = 0;
    if (!IEC61850::DecodeMmsGetNameListRequest(mmsPdu, &invokeId, &decoded)
             .ok()) {
      return {};
    }
    if (decoded.objectClass == IEC61850::MmsObjectClass::DOMAIN) {
      return {MakeNameListResponse(invokeId, {"IED1LD0"})};
    }
    if (decoded.objectClass == IEC61850::MmsObjectClass::NAMED_VARIABLE) {
      return {MakeNameListResponse(invokeId,
                                   {"LLN0", "LLN0$Beh$stVal"})};
    }
    if (decoded.objectClass ==
        IEC61850::MmsObjectClass::NAMED_VARIABLE_LIST) {
      return {MakeNameListResponse(invokeId, {"LLN0$ds1"})};
    }
    return {MakeNameListResponse(invokeId, {})};
  }
  if (request.serviceTag == 6) {
    return {MakeBooleanAttributesResponse(request.invokeId)};
  }
  if (request.serviceTag == 12) {
    return {MakeNamedVariableListResponse(request.invokeId, "IED1LD0",
                                          "LLN0$Beh$stVal")};
  }
  return {};
}

std::vector<std::uint8_t> WrapRawMmsPdu(
    std::span<const std::uint8_t> mmsPdu) {
  std::array<std::uint8_t, 4096> presentationBuffer{};
  std::size_t presentationSize = 0;
  if (!IEC61850::EncodeMmsPresentationData(mmsPdu, presentationBuffer,
                                            &presentationSize)
           .ok()) {
    return {};
  }
  std::array<std::uint8_t, 4096> sessionBuffer{};
  std::size_t sessionSize = 0;
  if (!IEC61850::EncodeIsoSessionData(
           std::span<const std::uint8_t>(presentationBuffer.data(),
                                         presentationSize),
           sessionBuffer, &sessionSize)
           .ok()) {
    return {};
  }
  return std::vector<std::uint8_t>(sessionBuffer.begin(),
                                  sessionBuffer.begin() + sessionSize);
}

bool AppendEncodedData(std::vector<std::uint8_t>* output,
                       grpc::Status status,
                       const std::vector<std::uint8_t>& encoded) {
  if (!status.ok()) {
    return false;
  }
  output->insert(output->end(), encoded.begin(), encoded.end());
  return true;
}

std::vector<std::uint8_t> MakeRcbDataStructure(
    std::string_view reportId = "RPT1") {
  std::vector<std::uint8_t> fields;
  std::vector<std::uint8_t> encoded;
  if (!AppendEncodedData(&fields,
                         IEC61850::EncodeMmsDataVisibleString(reportId,
                                                               &encoded),
                         encoded)) {
    return {};
  }
  if (!AppendEncodedData(&fields,
                         IEC61850::EncodeMmsDataBoolean(false, &encoded),
                         encoded)) {
    return {};
  }
  if (!AppendEncodedData(&fields,
                         IEC61850::EncodeMmsDataBoolean(false, &encoded),
                         encoded)) {
    return {};
  }
  if (!AppendEncodedData(&fields,
                         IEC61850::EncodeMmsDataVisibleString(
                             "IED1LD0/LLN0$ds1", &encoded),
                         encoded)) {
    return {};
  }
  if (!AppendEncodedData(&fields,
                         IEC61850::EncodeMmsDataUnsigned(7, &encoded),
                         encoded)) {
    return {};
  }
  const std::array<std::uint8_t, 2> optionalFields{0x50, 0x80};
  if (!AppendEncodedData(
          &fields,
          IEC61850::EncodeMmsDataBitString(
              6, optionalFields, &encoded),
          encoded)) {
    return {};
  }
  if (!AppendEncodedData(&fields,
                         IEC61850::EncodeMmsDataUnsigned(20, &encoded),
                         encoded)) {
    return {};
  }
  if (!AppendEncodedData(&fields,
                         IEC61850::EncodeMmsDataUnsigned(0, &encoded),
                         encoded)) {
    return {};
  }
  const std::array<std::uint8_t, 1> triggerOptions{0x44};
  if (!AppendEncodedData(
          &fields,
          IEC61850::EncodeMmsDataBitString(2, triggerOptions, &encoded),
          encoded)) {
    return {};
  }
  if (!AppendEncodedData(&fields,
                         IEC61850::EncodeMmsDataUnsigned(5000, &encoded),
                         encoded)) {
    return {};
  }
  if (!AppendEncodedData(&fields,
                         IEC61850::EncodeMmsDataBoolean(false, &encoded),
                         encoded)) {
    return {};
  }
  std::vector<std::uint8_t> structure;
  AppendTlv(&structure, 0xa2, fields);
  return structure;
}

std::vector<std::uint8_t> MakeRcbReadResponse(
    std::uint32_t invokeId, std::string_view reportId = "RPT1") {
  IEC61850::MmsReadResponse response;
  auto& item = response.items.emplace_back();
  item.success = true;
  item.encodedData = MakeRcbDataStructure(reportId);
  std::array<std::uint8_t, 4096> mmsBuffer{};
  std::size_t mmsSize = 0;
  if (item.encodedData.empty() ||
      !IEC61850::EncodeMmsReadResponse(invokeId, response, mmsBuffer,
                                       &mmsSize)
           .ok()) {
    return {};
  }
  return WrapRawMmsPdu(
      std::span<const std::uint8_t>(mmsBuffer.data(), mmsSize));
}

std::vector<std::uint8_t> MakeWriteResponse(std::uint32_t invokeId,
                                            std::size_t itemCount) {
  IEC61850::MmsWriteResponse response;
  response.items.resize(itemCount);
  for (auto& item : response.items) {
    item.success = true;
  }
  std::array<std::uint8_t, 4096> mmsBuffer{};
  std::size_t mmsSize = 0;
  if (itemCount == 0 ||
      !IEC61850::EncodeMmsWriteResponse(invokeId, response, mmsBuffer,
                                        &mmsSize)
           .ok()) {
    return {};
  }
  return WrapRawMmsPdu(
      std::span<const std::uint8_t>(mmsBuffer.data(), mmsSize));
}

std::vector<std::uint8_t> MakeBooleanReadResponse(std::uint32_t invokeId) {
  IEC61850::MmsReadResponse response;
  auto& item = response.items.emplace_back();
  item.success = true;
  if (!IEC61850::EncodeMmsDataBoolean(true, &item.encodedData).ok()) {
    return {};
  }
  std::array<std::uint8_t, 4096> mmsBuffer{};
  std::size_t mmsSize = 0;
  if (!IEC61850::EncodeMmsReadResponse(invokeId, response, mmsBuffer,
                                       &mmsSize)
           .ok()) {
    return {};
  }
  return WrapRawMmsPdu(
      std::span<const std::uint8_t>(mmsBuffer.data(), mmsSize));
}

std::vector<std::uint8_t> MakeVisibleStringReadResponse(
    std::uint32_t invokeId, std::string_view value) {
  IEC61850::MmsReadResponse response;
  auto& item = response.items.emplace_back();
  item.success = true;
  if (!IEC61850::EncodeMmsDataVisibleString(value, &item.encodedData).ok()) {
    return {};
  }
  std::array<std::uint8_t, 4096> mmsBuffer{};
  std::size_t mmsSize = 0;
  if (!IEC61850::EncodeMmsReadResponse(invokeId, response, mmsBuffer,
                                       &mmsSize)
           .ok()) {
    return {};
  }
  return WrapRawMmsPdu(
      std::span<const std::uint8_t>(mmsBuffer.data(), mmsSize));
}

std::vector<std::uint8_t> MakeEncodedReadResponse(
    std::uint32_t invokeId, std::vector<std::uint8_t> encodedValue) {
  IEC61850::MmsReadResponse response;
  auto& item = response.items.emplace_back();
  item.success = true;
  item.encodedData = std::move(encodedValue);
  std::array<std::uint8_t, 4096> mmsBuffer{};
  std::size_t mmsSize = 0;
  if (item.encodedData.empty() ||
      !IEC61850::EncodeMmsReadResponse(invokeId, response, mmsBuffer,
                                       &mmsSize)
           .ok()) {
    return {};
  }
  return WrapRawMmsPdu(
      std::span<const std::uint8_t>(mmsBuffer.data(), mmsSize));
}

std::vector<std::uint8_t> MakeSignedReadResponse(std::uint32_t invokeId,
                                                 std::int64_t value) {
  std::vector<std::uint8_t> encoded;
  if (!IEC61850::EncodeMmsDataSigned(value, &encoded).ok()) {
    return {};
  }
  return MakeEncodedReadResponse(invokeId, std::move(encoded));
}

std::vector<std::uint8_t> MakeUnsignedReadResponse(std::uint32_t invokeId,
                                                    std::uint64_t value) {
  std::vector<std::uint8_t> encoded;
  if (!IEC61850::EncodeMmsDataUnsigned(value, &encoded).ok()) {
    return {};
  }
  return MakeEncodedReadResponse(invokeId, std::move(encoded));
}

std::vector<std::uint8_t> MakeDomainObjectName(
    std::string_view domain, std::string_view identifier) {
  std::vector<std::uint8_t> object;
  AppendTlv(&object, 0x1a,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(domain.data()),
                domain.size()));
  AppendTlv(&object, 0x1a,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(identifier.data()),
                identifier.size()));
  std::vector<std::uint8_t> result;
  AppendTlv(&result, 0xa1, object);
  return result;
}

std::vector<std::uint8_t> MakeVariableSpecification(
    const std::vector<std::uint8_t>& objectName) {
  std::vector<std::uint8_t> specification;
  AppendTlv(&specification, 0xa0, objectName);
  std::vector<std::uint8_t> result;
  AppendTlv(&result, 0x30, specification);
  return result;
}

std::vector<std::uint8_t> MakeVmdVariableSpecification(
    std::string_view identifier) {
  std::vector<std::uint8_t> object;
  AppendTlv(&object, 0x80,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(identifier.data()),
                identifier.size()));
  return MakeVariableSpecification(object);
}

std::vector<std::uint8_t> MakeLastApplErrorData(
    std::string_view controlReference, std::uint8_t controlNumber) {
  std::vector<std::uint8_t> origin;
  const std::array<std::uint8_t, 1> originCategory{2};
  const std::array<std::uint8_t, 4> originIdentifier{1, 2, 3, 4};
  AppendTlv(&origin, 0x85, originCategory);
  AppendTlv(&origin, 0x89, originIdentifier);

  std::vector<std::uint8_t> data;
  AppendTlv(&data, 0x8a,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(controlReference.data()),
                controlReference.size()));
  const std::array<std::uint8_t, 1> error{1};
  AppendTlv(&data, 0x85, error);
  AppendTlv(&data, 0xa2, origin);
  const std::array<std::uint8_t, 1> ctlNum{controlNumber};
  AppendTlv(&data, 0x86, ctlNum);
  const std::array<std::uint8_t, 1> addCause{10};
  AppendTlv(&data, 0x85, addCause);

  std::vector<std::uint8_t> result;
  AppendTlv(&result, 0xa2, data);
  return result;
}

std::vector<std::uint8_t> MakeCommandTerminationReport(
    std::uint8_t controlNumber, bool includeError = false) {
  std::vector<std::uint8_t> variableList;
  std::vector<std::uint8_t> values;
  if (includeError) {
    AppendEncoded(&variableList,
                  MakeVmdVariableSpecification("LastApplError"));
    AppendEncoded(&values, MakeLastApplErrorData(
                              "IED1LD0/CSWI1$Pos$Oper", controlNumber));
  }
  AppendEncoded(&variableList, MakeVariableSpecification(
                                   MakeDomainObjectName(
                                       "IED1LD0", "CSWI1$Pos$Oper")));
  const std::array<std::uint8_t, 1> trueValue{0xff};
  AppendTlv(&values, 0x83, trueValue);

  std::vector<std::uint8_t> report;
  AppendTlv(&report, 0xa0, variableList);
  AppendTlv(&report, 0xa1, values);
  std::vector<std::uint8_t> outer;
  AppendTlv(&outer, 0xa0, report);
  std::vector<std::uint8_t> unconfirmed;
  AppendTlv(&unconfirmed, 0xa3, outer);
  return WrapRawMmsPdu(unconfirmed);
}

ScriptedResponses MakeControlResponse(
    std::span<const std::uint8_t> payload, std::size_t /*sendCount*/) {
  IEC61850::IsoSessionPduView sessionPdu;
  if (!IEC61850::DecodeIsoSessionPdu(payload, &sessionPdu).ok() ||
      sessionPdu.type != IEC61850::IsoSessionPduType::DATA) {
    return {};
  }
  std::span<const std::uint8_t> mmsPdu;
  if (!IEC61850::DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu)
           .ok()) {
    return {};
  }
  IEC61850::MmsConfirmedPduView request;
  if (!IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok()) {
    return {};
  }
  if (request.serviceTag == 1) {
    IEC61850::MmsGetNameListRequest nameList;
    std::uint32_t invokeId = 0;
    if (!IEC61850::DecodeMmsGetNameListRequest(mmsPdu, &invokeId, &nameList)
             .ok()) {
      return {};
    }
    return {MakeEmptyNameListResponse(invokeId)};
  }
  if (request.serviceTag == 4) {
    return {MakeBooleanReadResponse(request.invokeId)};
  }
  if (request.serviceTag == 5) {
    IEC61850::MmsWriteRequest write;
    std::uint32_t invokeId = 0;
    if (!IEC61850::DecodeMmsWriteRequest(mmsPdu, &invokeId, &write).ok()) {
      return {};
    }
    return {MakeWriteResponse(invokeId, write.items.size())};
  }
  return {};
}

ScriptedResponses MakeControlDirectoryResponse(
    std::span<const std::uint8_t> payload, std::size_t /*sendCount*/) {
  IEC61850::IsoSessionPduView sessionPdu;
  if (!IEC61850::DecodeIsoSessionPdu(payload, &sessionPdu).ok() ||
      sessionPdu.type != IEC61850::IsoSessionPduType::DATA) {
    return {};
  }
  std::span<const std::uint8_t> mmsPdu;
  if (!IEC61850::DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu)
           .ok()) {
    return {};
  }
  IEC61850::MmsConfirmedPduView request;
  if (!IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok()) {
    return {};
  }
  if (request.serviceTag == 1) {
    IEC61850::MmsGetNameListRequest nameList;
    std::uint32_t invokeId = 0;
    if (!IEC61850::DecodeMmsGetNameListRequest(mmsPdu, &invokeId, &nameList)
             .ok()) {
      return {};
    }
    if (nameList.objectClass == IEC61850::MmsObjectClass::DOMAIN) {
      return {MakeNameListResponse(invokeId, {"IED1LD0"})};
    }
    if (nameList.objectClass == IEC61850::MmsObjectClass::NAMED_VARIABLE) {
      return {MakeNameListResponse(
          invokeId, {"CSWI1$Pos$ctlVal", "CSWI1$Pos$SBO",
                     "CSWI1$Pos$SBOw",
                     "CSWI1$Pos$Oper", "CSWI1$Pos$Cancel"})};
    }
    return {MakeNameListResponse(invokeId, {})};
  }
  if (request.serviceTag == 6) {
    return {MakeBooleanAttributesResponse(request.invokeId)};
  }
  if (request.serviceTag == 4) {
    return {MakeVisibleStringReadResponse(request.invokeId,
                                          "IED1LD0/CSWI1.Pos")};
  }
  if (request.serviceTag == 5) {
    IEC61850::MmsWriteRequest write;
    std::uint32_t invokeId = 0;
    if (!IEC61850::DecodeMmsWriteRequest(mmsPdu, &invokeId, &write).ok()) {
      return {};
    }
    return {MakeWriteResponse(invokeId, write.items.size())};
  }
  return {};
}

ScriptedResponses MakeEnhancedControlResponse(
    std::span<const std::uint8_t> payload, std::size_t /*sendCount*/,
    std::int64_t ctlModel, bool includeTermination, bool includeError,
    bool includeCancel = true) {
  IEC61850::IsoSessionPduView sessionPdu;
  if (!IEC61850::DecodeIsoSessionPdu(payload, &sessionPdu).ok() ||
      sessionPdu.type != IEC61850::IsoSessionPduType::DATA) {
    return {};
  }
  std::span<const std::uint8_t> mmsPdu;
  if (!IEC61850::DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu)
           .ok()) {
    return {};
  }
  IEC61850::MmsConfirmedPduView request;
  if (!IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok()) {
    return {};
  }
  if (request.serviceTag == 1) {
    IEC61850::MmsGetNameListRequest nameList;
    std::uint32_t invokeId = 0;
    if (!IEC61850::DecodeMmsGetNameListRequest(mmsPdu, &invokeId, &nameList)
             .ok()) {
      return {};
    }
    if (nameList.objectClass == IEC61850::MmsObjectClass::DOMAIN) {
      return {MakeNameListResponse(invokeId, {"IED1LD0"})};
    }
    if (nameList.objectClass == IEC61850::MmsObjectClass::NAMED_VARIABLE) {
      std::vector<std::string> variables{
          "CSWI1$Pos$ctlVal", "CSWI1$Pos$ctlModel",
          "CSWI1$Pos$sboTimeout", "CSWI1$Pos$operTimeout",
          "CSWI1$Pos$Oper"};
      if (ctlModel == 4) {
        variables.emplace_back("CSWI1$Pos$SBOw");
        if (includeCancel) {
          variables.emplace_back("CSWI1$Pos$Cancel");
        }
      }
      return {MakeNameListResponse(invokeId, variables)};
    }
    return {MakeNameListResponse(invokeId, {})};
  }
  if (request.serviceTag == 6) {
    IEC61850::MmsObjectName object;
    if (!DecodeRequestedVariable(request, &object)) {
      return {};
    }
    if (object.identifier.ends_with("$ctlVal")) {
      return {MakeTypedAttributesResponse(
          request.invokeId, IEC61850::MmsTypeSpecificationKind::BOOLEAN, 1)};
    }
    if (object.identifier.ends_with("$ctlModel")) {
      return {MakeTypedAttributesResponse(
          request.invokeId, IEC61850::MmsTypeSpecificationKind::INTEGER)};
    }
    if (object.identifier.ends_with("$sboTimeout") ||
        object.identifier.ends_with("$operTimeout")) {
      return {MakeTypedAttributesResponse(
          request.invokeId, IEC61850::MmsTypeSpecificationKind::UNSIGNED)};
    }
    return {MakeBooleanAttributesResponse(request.invokeId)};
  }
  if (request.serviceTag == 4) {
    IEC61850::MmsReadRequest read;
    std::uint32_t invokeId = 0;
    if (!IEC61850::DecodeMmsReadRequest(mmsPdu, &invokeId, &read).ok() ||
        read.variables.empty()) {
      return {};
    }
    if (read.variables.front().identifier.ends_with("$SBO")) {
      return {MakeVisibleStringReadResponse(invokeId, "IED1LD0/CSWI1.Pos")};
    }
    if (read.variables.front().identifier.ends_with("$ctlModel")) {
      return {MakeSignedReadResponse(invokeId, ctlModel)};
    }
    if (read.variables.front().identifier.ends_with("$sboTimeout") ||
        read.variables.front().identifier.ends_with("$operTimeout")) {
      return {MakeUnsignedReadResponse(invokeId, 100)};
    }
    return {MakeBooleanReadResponse(invokeId)};
  }
  if (request.serviceTag == 5) {
    IEC61850::MmsWriteRequest write;
    std::uint32_t invokeId = 0;
    if (!IEC61850::DecodeMmsWriteRequest(mmsPdu, &invokeId, &write).ok()) {
      return {};
    }
    const bool isOperate = std::ranges::any_of(
        write.items, [](const auto& item) {
          return item.variable.identifier.ends_with("$Oper");
        });
    ScriptedResponses responses;
    responses.emplace_back(MakeWriteResponse(invokeId, write.items.size()));
    if (isOperate && includeTermination) {
      responses.emplace_back(MakeCommandTerminationReport(1, includeError));
    }
    return responses;
  }
  return {};
}

ScriptedResponses MakeControlDirectoryResponseWithExtraWriteResult(
    std::span<const std::uint8_t> payload, std::size_t sendCount) {
  IEC61850::IsoSessionPduView sessionPdu;
  std::span<const std::uint8_t> mmsPdu;
  IEC61850::MmsConfirmedPduView request;
  if (IEC61850::DecodeIsoSessionPdu(payload, &sessionPdu).ok() &&
      sessionPdu.type == IEC61850::IsoSessionPduType::DATA &&
      IEC61850::DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu)
          .ok() &&
      IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok() &&
      request.serviceTag == 5) {
    IEC61850::MmsWriteRequest write;
    std::uint32_t invokeId = 0;
    if (!IEC61850::DecodeMmsWriteRequest(mmsPdu, &invokeId, &write).ok()) {
      return {};
    }
    return {MakeWriteResponse(invokeId, write.items.size() + 1)};
  }
  return MakeControlDirectoryResponse(payload, sendCount);
}

std::vector<std::uint8_t> MakeReportWithReason(std::uint8_t reasonByte,
                                              std::uint64_t confRev = 7,
                                              std::string_view reportId = "RPT1") {
  std::vector<std::uint8_t> accessResults;
  std::vector<std::uint8_t> encoded;
  if (!AppendEncodedData(&accessResults,
                         IEC61850::EncodeMmsDataVisibleString(reportId,
                                                               &encoded),
                         encoded)) {
    return {};
  }
  const std::array<std::uint8_t, 2> optionalFields{0x50, 0x80};
  if (!AppendEncodedData(
          &accessResults,
          IEC61850::EncodeMmsDataBitString(6, optionalFields, &encoded),
          encoded)) {
    return {};
  }
  if (!AppendEncodedData(&accessResults,
                         IEC61850::EncodeMmsDataUnsigned(1, &encoded),
                         encoded)) {
    return {};
  }
  if (!AppendEncodedData(&accessResults,
                         IEC61850::EncodeMmsDataUnsigned(confRev, &encoded),
                         encoded)) {
    return {};
  }
  const std::array<std::uint8_t, 1> inclusion{0x80};
  if (!AppendEncodedData(
          &accessResults,
          IEC61850::EncodeMmsDataBitString(7, inclusion, &encoded),
          encoded)) {
    return {};
  }
  if (!AppendEncodedData(&accessResults,
                         IEC61850::EncodeMmsDataBoolean(true, &encoded),
                         encoded)) {
    return {};
  }
  const std::array<std::uint8_t, 1> reason{reasonByte};
  if (!AppendEncodedData(
          &accessResults,
          IEC61850::EncodeMmsDataBitString(3, reason, &encoded),
          encoded)) {
    return {};
  }

  std::vector<std::uint8_t> variableName;
  const std::array<std::uint8_t, 3> reportVariableName{'R', 'P', 'T'};
  AppendTlv(&variableName, 0x80, reportVariableName);
  std::vector<std::uint8_t> variableListName;
  AppendTlv(&variableListName, 0xa1, variableName);
  std::vector<std::uint8_t> accessList;
  AppendTlv(&accessList, 0xa0, accessResults);
  std::vector<std::uint8_t> report;
  report.insert(report.end(), variableListName.begin(), variableListName.end());
  report.insert(report.end(), accessList.begin(), accessList.end());
  std::vector<std::uint8_t> reportChoice;
  AppendTlv(&reportChoice, 0xa0, report);
  std::vector<std::uint8_t> unconfirmed;
  AppendTlv(&unconfirmed, 0xa3, reportChoice);
  return WrapRawMmsPdu(unconfirmed);
}

std::vector<std::uint8_t> MakeGeneralInterrogationReport(
    std::string_view reportId = "RPT1") {
  return MakeReportWithReason(0x08, 7, reportId);
}

std::vector<std::uint8_t> MakeOrdinaryReport() {
  return MakeReportWithReason(0x00);
}

// 将CommandTermination放在Write确认之前，验证确认交换的暂存路径。
ScriptedResponses MakeEnhancedControlResponseTerminationFirst(
    std::span<const std::uint8_t> payload, std::size_t sendCount) {
  auto responses =
      MakeEnhancedControlResponse(payload, sendCount, 3, true, false);
  if (responses.size() == 2u) {
    std::swap(responses.front(), responses.back());
  }
  return responses;
}

// 在Write确认和CommandTermination之间插入普通报告，验证报告与控制终止交错。
ScriptedResponses MakeEnhancedControlResponseWithOrdinaryReport(
    std::span<const std::uint8_t> payload, std::size_t sendCount) {
  auto responses =
      MakeEnhancedControlResponse(payload, sendCount, 3, true, false);
  if (responses.size() == 2u) {
    responses.insert(responses.begin() + 1, MakeOrdinaryReport());
  }
  return responses;
}

ScriptedResponses MakeRcbResponseInternal(
    std::span<const std::uint8_t> payload, std::size_t /*sendCount*/,
    bool duplicateGeneralInterrogation,
    bool prependOrdinaryReport,
    bool invalidConfRevGeneralInterrogation) {
  IEC61850::IsoSessionPduView sessionPdu;
  if (!IEC61850::DecodeIsoSessionPdu(payload, &sessionPdu).ok() ||
      sessionPdu.type != IEC61850::IsoSessionPduType::DATA) {
    return {};
  }
  std::span<const std::uint8_t> mmsPdu;
  if (!IEC61850::DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu)
           .ok()) {
    return {};
  }
  IEC61850::MmsConfirmedPduView request;
  if (!IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok()) {
    return {};
  }
  if (request.serviceTag == 1 || request.serviceTag == 6 ||
      request.serviceTag == 12) {
    // 复用在线目录脚本，仅将变量和DataSet列表扩展为RCB计划所需对象。
    IEC61850::MmsGetNameListRequest nameList;
    std::uint32_t invokeId = 0;
    if (request.serviceTag == 1 &&
        IEC61850::DecodeMmsGetNameListRequest(mmsPdu, &invokeId, &nameList)
            .ok()) {
      if (nameList.objectClass == IEC61850::MmsObjectClass::DOMAIN) {
        return {MakeNameListResponse(invokeId, {"IED1LD0"})};
      }
      if (nameList.objectClass == IEC61850::MmsObjectClass::NAMED_VARIABLE) {
        return {MakeNameListResponse(
            invokeId, {"LLN0", "LLN0$Beh$stVal", "LLN0$BR$brcb1",
                       "LLN0$UR$urcb1"})};
      }
      return {MakeNameListResponse(invokeId, {"LLN0$ds1"})};
    }
    if (request.serviceTag == 6) {
      return {MakeBooleanAttributesResponse(request.invokeId)};
    }
    if (request.serviceTag == 12) {
      return {MakeNamedVariableListResponse(request.invokeId, "IED1LD0",
                                            "LLN0$Beh$stVal")};
    }
  }
  if (request.serviceTag == 4) {
    IEC61850::MmsReadRequest read;
    std::uint32_t invokeId = 0;
    if (!IEC61850::DecodeMmsReadRequest(mmsPdu, &invokeId, &read).ok() ||
        read.variables.empty()) {
      return {};
    }
    const auto reportId =
        read.variables.front().identifier.ends_with("$UR$urcb1") ? "RPT2"
                                                                  : "RPT1";
    return {MakeRcbReadResponse(invokeId, reportId)};
  }
  if (request.serviceTag == 5) {
    IEC61850::MmsWriteRequest write;
    std::uint32_t invokeId = 0;
    if (!IEC61850::DecodeMmsWriteRequest(mmsPdu, &invokeId, &write).ok()) {
      return {};
    }
    bool generalInterrogation = false;
    for (const auto& item : write.items) {
      generalInterrogation = generalInterrogation ||
                             item.variable.identifier.ends_with("$GI");
    }
    if (generalInterrogation) {
      ScriptedResponses responses;
      if (prependOrdinaryReport) {
        responses.emplace_back(MakeOrdinaryReport());
      }
      const auto reportId =
          std::any_of(write.items.begin(), write.items.end(), [](const auto& item) {
            return item.variable.identifier.ends_with("$UR$urcb1$GI");
          })
              ? "RPT2"
              : "RPT1";
      responses.emplace_back(
          invalidConfRevGeneralInterrogation
              ? MakeReportWithReason(0x08, 8, reportId)
              : MakeGeneralInterrogationReport(reportId));
      if (duplicateGeneralInterrogation) {
        responses.emplace_back(MakeGeneralInterrogationReport(reportId));
      }
      responses.emplace_back(MakeWriteResponse(invokeId, write.items.size()));
      return responses;
    }
    return {MakeWriteResponse(invokeId, write.items.size())};
  }
  return {};
}

ScriptedResponses MakeRcbResponseWithDuplicateGi(
    std::span<const std::uint8_t> payload, std::size_t sendCount) {
  return MakeRcbResponseInternal(payload, sendCount, true, false, false);
}

ScriptedResponses MakeRcbResponse(std::span<const std::uint8_t> payload,
                                  std::size_t sendCount) {
  return MakeRcbResponseInternal(payload, sendCount, false, false, false);
}

ScriptedResponses MakeRcbResponseWithPreReadyOrdinaryReport(
    std::span<const std::uint8_t> payload, std::size_t sendCount) {
  return MakeRcbResponseInternal(payload, sendCount, false, true, false);
}

ScriptedResponses MakeRcbResponseWithInvalidConfRevGi(
    std::span<const std::uint8_t> payload, std::size_t sendCount) {
  return MakeRcbResponseInternal(payload, sendCount, false, false, true);
}

class ScriptedTransport final : public IEC61850::MmsTransport {
public:
  using ResponseBuilder =
      std::function<ScriptedResponses(std::span<const std::uint8_t>,
                                      std::size_t)>;
  using ResponseDropPredicate =
      std::function<bool(std::span<const std::uint8_t>)>;

  explicit ScriptedTransport(std::shared_ptr<FactoryState> state,
                             ResponseBuilder responseBuilder = {},
                             bool includeWrite = false,
                             IEC61850Proto::NetworkChannel channel =
                                 IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED,
                             bool failWhenIdle = false,
                             ResponseDropPredicate responseDropPredicate = {},
                             std::shared_ptr<SendGate> sendGate = {})
      : state_(std::move(state)),
        responseBuilder_(std::move(responseBuilder)),
        includeWrite_(includeWrite),
        channel_(channel),
        failWhenIdle_(failWhenIdle),
        responseDropPredicate_(std::move(responseDropPredicate)),
        sendGate_(std::move(sendGate)) {}

  grpc::Status Connect(
      const IEC61850::MmsTransportEndpoint& endpoint,
      std::uint32_t timeoutMs = 0) override {
    std::chrono::milliseconds connectDelay;
    {
      std::lock_guard lock(state_->mutex);
      ++state_->connectCalls;
      if (channel_ == IEC61850Proto::NETWORK_CHANNEL_A) {
        ++state_->connectCallsA;
      } else if (channel_ == IEC61850Proto::NETWORK_CHANNEL_B) {
        ++state_->connectCallsB;
      }
      sessionIndex_ = state_->sessionSendCounts.size();
      state_->sessionSendCounts.emplace_back(0);
      state_->sessionChannels.emplace_back(channel_);
      state_->connectTimeouts.emplace_back(timeoutMs);
      connectDelay = state_->connectDelay;
      connected_ = true;
      received_.push_back(MakeSessionAccept(includeWrite_));
      state_->condition.notify_all();
    }
    endpoint_ = endpoint;
    if (connectDelay > std::chrono::milliseconds::zero()) {
      std::this_thread::sleep_for(connectDelay);
    }
    return grpc::Status::OK;
  }

  grpc::Status Send(std::span<const std::uint8_t> payload) override {
    if (sendGate_ != nullptr) {
      std::unique_lock gateLock(sendGate_->mutex);
      if (sendGate_->enabled) {
        sendGate_->enabled = false;
        sendGate_->entered = true;
        sendGate_->condition.notify_all();
        sendGate_->condition.wait(gateLock,
                                  [this] { return sendGate_->released; });
      }
    }
    std::lock_guard lock(state_->mutex);
    if (!connected_) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                          "脚本传输尚未连接");
    }
    ++state_->sendCalls;
    if (channel_ == IEC61850Proto::NETWORK_CHANNEL_A) {
      ++state_->sendCallsA;
    } else if (channel_ == IEC61850Proto::NETWORK_CHANNEL_B) {
      ++state_->sendCallsB;
    }
    if (sessionIndex_ < state_->sessionSendCounts.size()) {
      ++state_->sessionSendCounts[sessionIndex_];
    }
    if (responseBuilder_) {
      auto responses = responseBuilder_(payload, state_->sendCalls);
      if (responseDropPredicate_ && responseDropPredicate_(payload)) {
        responses.clear();
      }
      for (auto& response : responses) {
        if (!response.empty()) {
          received_.push_back(std::move(response));
        }
      }
      if (!responses.empty()) {
        state_->condition.notify_all();
      }
    } else if (state_->sendCalls == 2) {
      received_.push_back(MakeEmptyNameListResponse());
      state_->condition.notify_all();
    }
    sent_.emplace_back(payload.begin(), payload.end());
    state_->condition.notify_all();
    return grpc::Status::OK;
  }

  grpc::Status Send(std::span<const std::uint8_t> payload,
                    std::uint32_t timeoutMs) override {
    {
      std::lock_guard lock(state_->mutex);
      state_->sendTimeouts.emplace_back(timeoutMs);
    }
    return Send(payload);
  }

  grpc::Status Receive(std::vector<std::uint8_t>* payload,
                       std::uint32_t timeoutMs) override {
    if (payload == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "脚本传输接收输出为空");
    }
    std::unique_lock lock(state_->mutex);
    state_->receiveTimeouts.emplace_back(timeoutMs);
    if (received_.empty()) {
      if (failWhenIdle_) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "脚本传输按测试策略主动断开");
      }
      state_->condition.wait_for(lock,
                                 std::chrono::milliseconds(std::min(timeoutMs,
                                                                    10u)),
                                 [this] { return !received_.empty(); });
    }
    if (received_.empty()) {
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                          "脚本传输等待超时");
    }
    *payload = std::move(received_.front());
    received_.pop_front();
    return grpc::Status::OK;
  }

  void Close() noexcept override {
    std::lock_guard lock(state_->mutex);
    if (connected_) {
      ++state_->closeCalls;
    }
    connected_ = false;
  }

  bool IsConnected() const noexcept override { return connected_; }

private:
  std::shared_ptr<FactoryState> state_;
  IEC61850::MmsTransportEndpoint endpoint_;
  std::deque<std::vector<std::uint8_t>> received_;
  std::vector<std::vector<std::uint8_t>> sent_;
  ResponseBuilder responseBuilder_;
  bool includeWrite_ = false;
  IEC61850Proto::NetworkChannel channel_ =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  bool failWhenIdle_ = false;
  ResponseDropPredicate responseDropPredicate_;
  std::shared_ptr<SendGate> sendGate_;
  std::size_t sessionIndex_ = std::numeric_limits<std::size_t>::max();
  bool connected_ = false;
};

IEC61850::ProtocolIedPlan MakeMinimalPlan() {
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("conn1");
  plan.config.set_ied_name("IED1");
  plan.config.set_access_point("AP1");
  plan.ied.set_name("IED1");
  return plan;
}

IEC61850::ProtocolIedPlan MakeControlPlan() {
  auto plan = MakeMinimalPlan();
  auto* object = plan.ied.add_data_objects();
  object->set_data_ref("IED1LD0/CSWI1.Pos");
  object->set_cdc("DPC");
  object->set_access_point("AP1");
  auto* attribute = plan.ied.add_data_attributes();
  attribute->set_data_ref("IED1LD0/CSWI1.Pos.ctlVal");
  attribute->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_CO);
  attribute->set_basic_type("BOOLEAN");
  attribute->set_access_point("AP1");
  return plan;
}

IEC61850::ProtocolIedPlan MakeEnhancedControlPlan() {
  auto plan = MakeControlPlan();
  for (const auto [ref, basicType] : {
           std::pair<const char*, const char*>{
               "IED1LD0/CSWI1.Pos.ctlModel", "INT32"},
           {"IED1LD0/CSWI1.Pos.sboTimeout", "INT32U"},
           {"IED1LD0/CSWI1.Pos.operTimeout", "INT32U"}}) {
    auto* attribute = plan.ied.add_data_attributes();
    attribute->set_data_ref(ref);
    attribute->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_CF);
    attribute->set_basic_type(basicType);
    attribute->set_access_point("AP1");
  }
  return plan;
}

std::vector<IEC61850::ProtocolNetworkBinding> MakeBindings() {
  IEC61850::ProtocolNetworkBinding binding;
  binding.channel.set_channel(IEC61850Proto::NETWORK_CHANNEL_A);
  binding.channel.set_enabled(true);
  binding.channel.set_interface_name("test0");
  binding.channel.set_local_ip("127.0.0.1");
  binding.channel.set_remote_ip("127.0.0.1");
  binding.channel.set_remote_port(102);
  return {std::move(binding)};
}

std::vector<IEC61850::ProtocolNetworkBinding> MakeBindingsAB() {
  auto bindings = MakeBindings();
  auto backup = bindings.front();
  backup.channel.set_channel(IEC61850Proto::NETWORK_CHANNEL_B);
  backup.channel.set_interface_name("test1");
  bindings.emplace_back(std::move(backup));
  return bindings;
}

IEC61850::ProtocolIedPlan MakeDirectoryPlan() {
  auto plan = MakeMinimalPlan();
  auto* node = plan.ied.add_logical_nodes();
  node->set_node_ref("IED1LD0/LLN0");
  auto* attribute = plan.ied.add_data_attributes();
  attribute->set_data_ref("IED1LD0/LLN0.Beh.stVal");
  attribute->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  attribute->set_basic_type("BOOLEAN");
  attribute->set_count(1);
  auto* dataSet = plan.ied.add_data_sets();
  dataSet->set_data_set_ref("IED1LD0/LLN0$ds1");
  auto* member = dataSet->add_members();
  member->set_data_ref(attribute->data_ref());
  member->set_fc(attribute->fc());
  return plan;
}

IEC61850::ProtocolIedPlan MakeRcbPlan() {
  auto plan = MakeDirectoryPlan();
  auto* report = plan.ied.add_report_controls();
  report->set_rcb_ref("IED1LD0/LLN0$BR$brcb1");
  report->set_data_set_ref("IED1LD0/LLN0$ds1");
  report->set_report_id("RPT1");
  report->set_buffered(false);
  report->set_config_revision(7);
  report->set_max_instances(1);
  report->set_buffer_time_ms(20);
  report->set_integrity_period_ms(5000);
  report->mutable_trigger_options()->set_data_change(true);
  report->mutable_trigger_options()->set_general_interrogation(true);
  report->mutable_optional_fields()->set_sequence_number(true);
  report->mutable_optional_fields()->set_reason_code(true);
  report->mutable_optional_fields()->set_config_revision(true);
  return plan;
}

IEC61850::ProtocolIedPlan MakeMultiRcbPlan() {
  auto plan = MakeRcbPlan();
  auto* second = plan.ied.add_report_controls();
  *second = plan.ied.report_controls(0);
  second->set_rcb_ref("IED1LD0/LLN0$UR$urcb1");
  second->set_report_id("RPT2");
  return plan;
}

struct ControlRequestLog {
  std::mutex mutex;
  std::vector<std::string> readIdentifiers;
  std::vector<std::string> writeIdentifiers;
};

IEC61850::MmsObjectName MakeControlObject() {
  IEC61850::MmsObjectName object;
  object.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
  object.domain = "IED1LD0";
  object.identifier = "CSWI1$Pos";
  return object;
}

IEC61850::MmsControlCommand MakeControlCommand(
    IEC61850::MmsControlOperation operation =
        IEC61850::MmsControlOperation::OPERATE) {
  IEC61850::MmsControlCommand command;
  command.operation = operation;
  command.controlObject = MakeControlObject();
  command.controlNumber = 1;
  command.originCategory = 2;
  command.originIdentifier = {'t', 'e', 's', 't'};
  command.timestampMs = 0;
  command.check = 0;
  EXPECT_TRUE(IEC61850::EncodeMmsDataBoolean(
                   true, &command.controlValue)
                  .ok());
  return command;
}

ScriptedResponses MakeControlResponseAndLog(
    std::span<const std::uint8_t> payload, std::size_t sendCount,
    const std::shared_ptr<ControlRequestLog>& log) {
  IEC61850::IsoSessionPduView sessionPdu;
  std::span<const std::uint8_t> mmsPdu;
  IEC61850::MmsConfirmedPduView request;
  if (IEC61850::DecodeIsoSessionPdu(payload, &sessionPdu).ok() &&
      sessionPdu.type == IEC61850::IsoSessionPduType::DATA &&
      IEC61850::DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu)
          .ok() &&
      IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok()) {
    if (request.serviceTag == 4) {
      IEC61850::MmsReadRequest decoded;
      std::uint32_t invokeId = 0;
      if (IEC61850::DecodeMmsReadRequest(mmsPdu, &invokeId, &decoded).ok()) {
        std::lock_guard lock(log->mutex);
        for (const auto& variable : decoded.variables) {
          log->readIdentifiers.emplace_back(variable.domain + "/" +
                                            variable.identifier);
        }
      }
    } else if (request.serviceTag == 5) {
      IEC61850::MmsWriteRequest decoded;
      std::uint32_t invokeId = 0;
      if (IEC61850::DecodeMmsWriteRequest(mmsPdu, &invokeId, &decoded).ok()) {
        std::lock_guard lock(log->mutex);
        for (const auto& item : decoded.items) {
          log->writeIdentifiers.emplace_back(item.variable.domain + "/" +
                                             item.variable.identifier);
        }
      }
    }
  }
  return MakeControlDirectoryResponse(payload, sendCount);
}


// 验证MMS工作器使用注入的脚本传输完成ISO ACCEPT、Initiate和空NameList建链。
TEST(IEC61850MmsWorkerTest, UsesInjectedTransportForMinimalSession) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeMinimalPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint& endpoint,
              IEC61850Proto::NetworkChannel channel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        state->channels.push_back(channel);
        state->endpoints.push_back(endpoint);
        return std::make_unique<ScriptedTransport>(state);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }
  worker.Stop();

  std::lock_guard lock(state->mutex);
  EXPECT_EQ(state->factoryCalls, 1u);
  EXPECT_EQ(state->connectCalls, 1u);
  EXPECT_GE(state->sendCalls, 2u);
  EXPECT_EQ(state->closeCalls, 1u);
  ASSERT_EQ(state->channels.size(), 1u);
  EXPECT_EQ(state->channels.front(), IEC61850Proto::NETWORK_CHANNEL_A);
  ASSERT_EQ(state->endpoints.size(), 1u);
  EXPECT_EQ(state->endpoints.front().interfaceName, "test0");
  EXPECT_EQ(state->endpoints.front().remoteIp, "127.0.0.1");
  EXPECT_EQ(state->endpoints.front().remotePort, 102);
}

// 验证MMS关联建立的TCP/COTP和Session确认阶段共享一份递减的总超时预算。
TEST(IEC61850MmsWorkerTest, UsesSingleDeadlineForAssociationSetup) {
  auto state = std::make_shared<FactoryState>();
  state->connectDelay = 30ms;
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeMinimalPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(state);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }
  worker.Stop();

  std::lock_guard lock(state->mutex);
  ASSERT_FALSE(state->connectTimeouts.empty());
  ASSERT_FALSE(state->sendTimeouts.empty());
  ASSERT_FALSE(state->receiveTimeouts.empty());
  EXPECT_GT(state->connectTimeouts.front(), 0u);
  EXPECT_GT(state->sendTimeouts.front(), 0u);
  EXPECT_GT(state->receiveTimeouts.front(), 0u);
  EXPECT_LT(state->sendTimeouts.front(), state->connectTimeouts.front());
  EXPECT_LE(state->receiveTimeouts.front(), state->sendTimeouts.front());
}

// 验证未启动或收到非法参数时，控制入口在进入传输队列前拒绝请求。
TEST(IEC61850MmsWorkerTest, RejectsControlBeforeReady) {
  IEC61850::MmsSessionWorker worker(MakeMinimalPlan(), MakeBindings(), {});

  IEC61850::MmsReadResponse selectResponse;
  const auto selectStatus =
      worker.SelectMmsControl(MakeControlObject(), &selectResponse);
  EXPECT_EQ(selectStatus.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(selectResponse.items.empty());
  EXPECT_EQ(worker.SelectMmsControl(MakeControlObject(), nullptr)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  auto command = MakeControlCommand();
  IEC61850::MmsWriteResponse controlResponse;
  const auto controlStatus =
      worker.WriteMmsControl(command, &controlResponse);
  EXPECT_EQ(controlStatus.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(controlResponse.items.empty());
  EXPECT_EQ(worker.WriteMmsControl(command, nullptr).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  command.controlValue.clear();
  EXPECT_EQ(worker.WriteMmsControl(command, &controlResponse).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  command = MakeControlCommand(IEC61850::MmsControlOperation::SELECT);
  EXPECT_EQ(worker.WriteMmsControl(command, &controlResponse).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  auto invalidObject = MakeControlObject();
  invalidObject.type = IEC61850::MmsObjectNameType::VMD_SPECIFIC;
  EXPECT_EQ(worker.SelectMmsControl(invalidObject, &selectResponse)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证DataCenter控制请求的零毫秒截止时间在进入MMS控制队列前生效。
TEST(IEC61850MmsWorkerTest, RejectsPointControlWhenRequestDeadlineExpired) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(state,
                                                   MakeControlDirectoryResponse,
                                                   true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  IEC61850::MmsPointControlCommand command;
  command.controlObject = MakeControlObject();
  command.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  command.boolValue = true;
  command.requestTimeout = std::chrono::milliseconds(0);
  IEC61850::MmsWriteResponse response;
  const auto status = worker.ExecuteMmsPointControl(command, &response);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_TRUE(response.items.empty());
  worker.Stop();
}

// 验证已发送的同步控制Write也使用请求截止时间，而不是固定五秒确认窗口。
TEST(IEC61850MmsWorkerTest, BoundsConfirmedControlExchangeByRequestDeadline) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };
  const auto dropWriteResponse =
      [](std::span<const std::uint8_t> payload) {
        IEC61850::IsoSessionPduView sessionPdu;
        std::span<const std::uint8_t> mmsPdu;
        IEC61850::MmsConfirmedPduView request;
        return IEC61850::DecodeIsoSessionPdu(payload, &sessionPdu).ok() &&
               sessionPdu.type == IEC61850::IsoSessionPduType::DATA &&
               IEC61850::DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu)
                   .ok() &&
               IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok() &&
               request.serviceTag == 5;
      };

  IEC61850::MmsSessionWorker worker(
      MakeControlPlan(), MakeBindings(), std::move(callbacks),
      [state, dropWriteResponse](const IEC61850::MmsTransportEndpoint&,
                                 IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state, MakeControlDirectoryResponse, true,
            IEC61850Proto::NETWORK_CHANNEL_A, false, dropWriteResponse);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  IEC61850::MmsPointControlCommand command;
  command.controlObject = MakeControlObject();
  command.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  command.boolValue = true;
  command.requestTimeout = std::chrono::milliseconds(30);
  IEC61850::MmsWriteResponse response;
  const auto started = std::chrono::steady_clock::now();
  const auto status = worker.ExecuteMmsPointControl(command, &response);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_LT(elapsed, std::chrono::milliseconds(500));
  EXPECT_TRUE(response.items.empty());
  worker.Stop();
}

// 验证确认交换中途收到取消状态时立即停止等待并返回CANCELLED。
TEST(IEC61850MmsWorkerTest, CancelsConfirmedControlExchangeWithSharedState) {
  auto state = std::make_shared<FactoryState>();
  auto cancellation = std::make_shared<std::atomic_bool>(false);
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };
  const auto dropWriteResponse =
      [](std::span<const std::uint8_t> payload) {
        IEC61850::IsoSessionPduView sessionPdu;
        std::span<const std::uint8_t> mmsPdu;
        IEC61850::MmsConfirmedPduView request;
        return IEC61850::DecodeIsoSessionPdu(payload, &sessionPdu).ok() &&
               sessionPdu.type == IEC61850::IsoSessionPduType::DATA &&
               IEC61850::DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu)
                   .ok() &&
               IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok() &&
               request.serviceTag == 5;
      };
  IEC61850::MmsSessionWorker worker(
      MakeControlPlan(), MakeBindings(), std::move(callbacks),
      [state, dropWriteResponse](const IEC61850::MmsTransportEndpoint&,
                                 IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state, MakeControlDirectoryResponse, true,
            IEC61850Proto::NETWORK_CHANNEL_A, false, dropWriteResponse);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  IEC61850::MmsPointControlCommand command;
  command.controlObject = MakeControlObject();
  command.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  command.boolValue = true;
  command.requestTimeout = std::chrono::seconds(2);
  command.cancellation = cancellation;
  IEC61850::MmsWriteResponse response;
  grpc::Status commandStatus;
  std::jthread commandThread([&] {
    commandStatus = worker.ExecuteMmsPointControl(command, &response);
  });
  std::this_thread::sleep_for(20ms);
  cancellation->store(true, std::memory_order_release);
  commandThread.join();

  EXPECT_EQ(commandStatus.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_TRUE(response.items.empty());
  worker.Stop();
}

// 验证停止工作器时，正在等待确认的MMS控制交换会观察stop_token并快速收敛。
TEST(IEC61850MmsWorkerTest, StopsWhileConfirmedControlExchangeIsWaiting) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  const auto dropWriteResponse =
      [](std::span<const std::uint8_t> payload) {
        IEC61850::IsoSessionPduView sessionPdu;
        std::span<const std::uint8_t> mmsPdu;
        IEC61850::MmsConfirmedPduView request;
        return IEC61850::DecodeIsoSessionPdu(payload, &sessionPdu).ok() &&
               sessionPdu.type == IEC61850::IsoSessionPduType::DATA &&
               IEC61850::DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu)
                   .ok() &&
               IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok() &&
               request.serviceTag == 5;
      };
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };
  IEC61850::MmsSessionWorker worker(
      MakeControlPlan(), MakeBindings(), std::move(callbacks),
      [state, dropWriteResponse](const IEC61850::MmsTransportEndpoint&,
                                 IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state, MakeControlDirectoryResponse, true,
            IEC61850Proto::NETWORK_CHANNEL_A, false, dropWriteResponse);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }
  const auto sendsBefore = [&] {
    std::lock_guard lock(state->mutex);
    return state->sendCalls;
  }();

  IEC61850::MmsPointControlCommand command;
  command.controlObject = MakeControlObject();
  command.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  command.boolValue = true;
  command.requestTimeout = std::chrono::seconds(5);
  IEC61850::MmsWriteResponse response;
  grpc::Status commandStatus;
  std::thread commandThread([&] {
    commandStatus = worker.ExecuteMmsPointControl(command, &response);
  });
  {
    std::unique_lock lock(state->mutex);
    const bool sendStarted = state->condition.wait_for(lock, 1s, [&] {
      return state->sendCalls > sendsBefore;
    });
    if (!sendStarted) {
      lock.unlock();
      worker.Stop();
      commandThread.join();
      ADD_FAILURE() << "控制请求未进入确认交换发送阶段";
      return;
    }
  }

  const auto started = std::chrono::steady_clock::now();
  worker.Stop();
  commandThread.join();

  EXPECT_EQ(commandStatus.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(1));
  EXPECT_TRUE(response.items.empty());
}

// 验证同步控制在普通SBO选择的确认等待期间收到取消时，不继续等待完整控制超时。
TEST(IEC61850MmsWorkerTest, CancelsDuringOrdinarySboSelection) {
  auto state = std::make_shared<FactoryState>();
  auto cancellation = std::make_shared<std::atomic_bool>(false);
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };
  const auto dropReadResponse =
      [](std::span<const std::uint8_t> payload) {
        IEC61850::IsoSessionPduView sessionPdu;
        std::span<const std::uint8_t> mmsPdu;
        IEC61850::MmsConfirmedPduView request;
        return IEC61850::DecodeIsoSessionPdu(payload, &sessionPdu).ok() &&
               sessionPdu.type == IEC61850::IsoSessionPduType::DATA &&
               IEC61850::DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu)
                   .ok() &&
               IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok() &&
               request.serviceTag == 4;
      };

  IEC61850::MmsSessionWorker worker(
      MakeControlPlan(), MakeBindings(), std::move(callbacks),
      [state, dropReadResponse](const IEC61850::MmsTransportEndpoint&,
                                IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state, MakeControlDirectoryResponse, true,
            IEC61850Proto::NETWORK_CHANNEL_A, false, dropReadResponse);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  const auto sendsBefore = [&] {
    std::lock_guard lock(state->mutex);
    return state->sendCalls;
  }();
  IEC61850::MmsPointControlCommand command;
  command.controlObject = MakeControlObject();
  command.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  command.boolValue = true;
  command.requestTimeout = std::chrono::seconds(2);
  command.cancellation = cancellation;
  IEC61850::MmsWriteResponse response;
  grpc::Status commandStatus;
  const auto started = std::chrono::steady_clock::now();
  std::thread commandThread([&] {
    commandStatus = worker.ExecuteMmsPointControl(command, &response);
  });

  bool readSent = false;
  {
    std::unique_lock lock(state->mutex);
    readSent = state->condition.wait_for(lock, 1s, [&] {
      return state->sendCalls > sendsBefore;
    });
  }
  if (!readSent) {
    cancellation->store(true, std::memory_order_release);
    commandThread.join();
    worker.Stop();
    ADD_FAILURE() << "普通SBO选择请求未进入MMS发送阶段";
    return;
  }
  cancellation->store(true, std::memory_order_release);
  commandThread.join();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_EQ(commandStatus.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_LT(elapsed, std::chrono::milliseconds(500));
  EXPECT_TRUE(response.items.empty());
  worker.Stop();
}

// 验证READY会话中的Select和Operate控制入口通过统一串行队列转发到正确对象。
TEST(IEC61850MmsWorkerTest, SelectsAndOperatesThroughReadyControlQueue) {
  auto state = std::make_shared<FactoryState>();
  auto requestLog = std::make_shared<ControlRequestLog>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeControlPlan(), MakeBindings(), std::move(callbacks),
      [state, requestLog](const IEC61850::MmsTransportEndpoint&,
                          IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state,
            [requestLog](std::span<const std::uint8_t> payload,
                         std::size_t sendCount) {
              return MakeControlResponseAndLog(payload, sendCount,
                                                requestLog);
            },
            true);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  IEC61850::MmsReadResponse selectResponse;
  ASSERT_TRUE(worker.SelectMmsControl(MakeControlObject(), &selectResponse)
                  .ok());
  ASSERT_EQ(selectResponse.items.size(), 1u);
  EXPECT_TRUE(selectResponse.items.front().success);
  std::vector<std::uint8_t> expectedSelection;
  ASSERT_TRUE(IEC61850::EncodeMmsDataVisibleString(
                  "IED1LD0/CSWI1.Pos", &expectedSelection)
                  .ok());
  EXPECT_EQ(selectResponse.items.front().encodedData, expectedSelection);

  IEC61850::MmsWriteResponse operateResponse;
  ASSERT_TRUE(worker.WriteMmsControl(MakeControlCommand(), &operateResponse)
                  .ok());
  ASSERT_EQ(operateResponse.items.size(), 1u);
  EXPECT_TRUE(operateResponse.items.front().success);

  worker.Stop();

  std::lock_guard lock(requestLog->mutex);
  ASSERT_EQ(requestLog->readIdentifiers.size(), 1u);
  EXPECT_EQ(requestLog->readIdentifiers.front(),
            "IED1LD0/CSWI1$Pos$SBO");
  ASSERT_EQ(requestLog->writeIdentifiers.size(), 1u);
  EXPECT_EQ(requestLog->writeIdentifiers.front(),
            "IED1LD0/CSWI1$Pos$Oper");
}

// 验证ctlModel=3的Oper只有在同一MMS会话收到成功CommandTermination后才完成。
TEST(IEC61850MmsWorkerTest, EnhancedDirectOperateWaitsForCommandTermination) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state,
            [](std::span<const std::uint8_t> payload, std::size_t sendCount) {
              return MakeEnhancedControlResponse(payload, sendCount, 3, true,
                                                  false);
            },
            true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  IEC61850::MmsWriteResponse response;
  ASSERT_TRUE(worker.WriteMmsControl(MakeControlCommand(), &response).ok());
  ASSERT_EQ(response.items.size(), 1u);
  EXPECT_TRUE(response.items.front().success);
  worker.Stop();
}

// 验证CommandTermination早于Write确认到达时会暂存并在确认后完成Oper。
TEST(IEC61850MmsWorkerTest, EnhancedDirectConsumesTerminationBeforeWriteAck) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state, MakeEnhancedControlResponseTerminationFirst, true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  IEC61850::MmsWriteResponse response;
  ASSERT_TRUE(worker.WriteMmsControl(MakeControlCommand(), &response).ok());
  ASSERT_EQ(response.items.size(), 1u);
  EXPECT_TRUE(response.items.front().success);
  worker.Stop();
}

// 验证ctlModel=3携带operTm时仍等待成功CommandTermination，而不是以Write确认提前结束。
TEST(IEC61850MmsWorkerTest, EnhancedTimedOperateWaitsForTermination) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state,
            [](std::span<const std::uint8_t> payload, std::size_t sendCount) {
              return MakeEnhancedControlResponse(payload, sendCount, 3, true,
                                                  false);
            },
            true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  auto command = MakeControlCommand();
  command.operateTimestampMs = 1234;
  IEC61850::MmsWriteResponse response;
  ASSERT_TRUE(worker.WriteMmsControl(command, &response).ok());
  ASSERT_EQ(response.items.size(), 1u);
  EXPECT_TRUE(response.items.front().success);
  worker.Stop();
}

// 验证普通InformationReport与CommandTermination交错时不会误完成或阻断当前Oper。
TEST(IEC61850MmsWorkerTest, EnhancedDirectProcessesInterleavedOrdinaryReport) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state, MakeEnhancedControlResponseWithOrdinaryReport, true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  IEC61850::MmsWriteResponse response;
  ASSERT_TRUE(worker.WriteMmsControl(MakeControlCommand(), &response).ok());
  ASSERT_EQ(response.items.size(), 1u);
  EXPECT_TRUE(response.items.front().success);
  worker.Stop();
}

// 验证ctlModel=4的SBOw选择在CommandTermination成功前保持，成功后不能再次Oper。
TEST(IEC61850MmsWorkerTest, EnhancedSboOperateClearsSelectionAfterTermination) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state,
            [](std::span<const std::uint8_t> payload, std::size_t sendCount) {
              return MakeEnhancedControlResponse(payload, sendCount, 4, true,
                                                  false);
            },
            true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  auto select = MakeControlCommand(
      IEC61850::MmsControlOperation::SELECT_WITH_VALUE);
  IEC61850::MmsWriteResponse selectResponse;
  ASSERT_TRUE(worker.WriteMmsControl(select, &selectResponse).ok());
  IEC61850::MmsWriteResponse operateResponse;
  ASSERT_TRUE(worker.WriteMmsControl(MakeControlCommand(), &operateResponse)
                  .ok());
  EXPECT_EQ(worker.WriteMmsControl(MakeControlCommand(), &operateResponse)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  worker.Stop();
}

// 验证同一对象的两个并发增强Oper只有一个进入远端Write，另一个立即被执行占用拒绝。
TEST(IEC61850MmsWorkerTest, ConcurrentEnhancedOperatesShareOneExecutionReservation) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state,
            [](std::span<const std::uint8_t> payload, std::size_t sendCount) {
              return MakeEnhancedControlResponse(payload, sendCount, 3, false,
                                                  false);
            },
            true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  const auto sendsBefore = [&] {
    std::lock_guard lock(state->mutex);
    return state->sendCalls;
  }();
  grpc::Status firstStatus;
  IEC61850::MmsWriteResponse firstResponse;
  std::thread firstRequest([&] {
    firstStatus = worker.WriteMmsControl(MakeControlCommand(), &firstResponse);
  });
  {
    std::unique_lock lock(state->mutex);
    ASSERT_TRUE(state->condition.wait_for(lock, 1s, [&] {
      return state->sendCalls > sendsBefore;
    }));
  }

  IEC61850::MmsWriteResponse secondResponse;
  const auto secondStatus =
      worker.WriteMmsControl(MakeControlCommand(), &secondResponse);
  EXPECT_EQ(secondStatus.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  {
    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->sendCalls, sendsBefore + 1);
  }

  firstRequest.join();
  EXPECT_EQ(firstStatus.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  worker.Stop();
}

// 验证增强安全Oper等待终止期间的显式Cancel会被同一收发线程优先处理。
TEST(IEC61850MmsWorkerTest, CancelsEnhancedOperateWhileTerminationPending) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state,
            [](std::span<const std::uint8_t> payload, std::size_t sendCount) {
              return MakeEnhancedControlResponse(payload, sendCount, 4, false,
                                                  false);
            },
            true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  auto select = MakeControlCommand(
      IEC61850::MmsControlOperation::SELECT_WITH_VALUE);
  IEC61850::MmsWriteResponse selectResponse;
  ASSERT_TRUE(worker.WriteMmsControl(select, &selectResponse).ok());

  const auto sendsBeforeOperate = [&] {
    std::lock_guard lock(state->mutex);
    return state->sendCalls;
  }();
  grpc::Status operateStatus;
  IEC61850::MmsWriteResponse operateResponse;
  std::thread operateRequest([&] {
    operateStatus = worker.WriteMmsControl(MakeControlCommand(),
                                           &operateResponse);
  });
  {
    std::unique_lock lock(state->mutex);
    ASSERT_TRUE(state->condition.wait_for(lock, 1s, [&] {
      return state->sendCalls > sendsBeforeOperate;
    }));
  }

  auto cancel = MakeControlCommand(IEC61850::MmsControlOperation::CANCEL);
  cancel.controlValue.clear();
  IEC61850::MmsWriteResponse cancelResponse;
  const auto cancelStatus = worker.WriteMmsControl(cancel, &cancelResponse);
  EXPECT_TRUE(cancelStatus.ok());
  operateRequest.join();
  EXPECT_EQ(operateStatus.error_code(), grpc::StatusCode::CANCELLED);
  IEC61850::MmsWriteResponse afterCancelResponse;
  EXPECT_EQ(worker.WriteMmsControl(MakeControlCommand(), &afterCancelResponse)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  worker.Stop();
}

// 验证停止会话功能期间，等待CommandTermination的Oper会被取消并及时收敛。
TEST(IEC61850MmsWorkerTest, StopsEnhancedOperateWaitingForTermination) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state,
            [](std::span<const std::uint8_t> payload, std::size_t sendCount) {
              return MakeEnhancedControlResponse(payload, sendCount, 3, false,
                                                  false);
            },
            true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  const auto sendsBeforeOperate = [&] {
    std::lock_guard lock(state->mutex);
    return state->sendCalls;
  }();
  grpc::Status operateStatus;
  IEC61850::MmsWriteResponse operateResponse;
  std::thread operateRequest([&] {
    operateStatus = worker.WriteMmsControl(MakeControlCommand(),
                                           &operateResponse);
  });
  {
    std::unique_lock lock(state->mutex);
    ASSERT_TRUE(state->condition.wait_for(lock, 1s, [&] {
      return state->sendCalls > sendsBeforeOperate;
    }));
  }

  worker.Stop();
  operateRequest.join();
  EXPECT_EQ(operateStatus.error_code(), grpc::StatusCode::CANCELLED);
}

// 验证增强安全CommandTermination的LastApplError保留SBO并要求先Cancel。
TEST(IEC61850MmsWorkerTest, EnhancedOperateFailureRequiresCancel) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state,
            [](std::span<const std::uint8_t> payload, std::size_t sendCount) {
              return MakeEnhancedControlResponse(payload, sendCount, 4, true,
                                                  true);
            },
            true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  auto select = MakeControlCommand(
      IEC61850::MmsControlOperation::SELECT_WITH_VALUE);
  IEC61850::MmsWriteResponse response;
  ASSERT_TRUE(worker.WriteMmsControl(select, &response).ok());
  const auto failure =
      worker.WriteMmsControl(MakeControlCommand(), &response);
  EXPECT_EQ(failure.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(worker.WriteMmsControl(MakeControlCommand(), &response)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  auto cancel = MakeControlCommand(IEC61850::MmsControlOperation::CANCEL);
  cancel.controlValue.clear();
  ASSERT_TRUE(worker.WriteMmsControl(cancel, &response).ok());
  worker.Stop();
}

// 验证ctlModel=3没有Cancel能力时，已核对的LastApplError只释放本地执行占用。
TEST(IEC61850MmsWorkerTest, DirectEnhancedFailureReleasesPendingOperation) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state,
            [](std::span<const std::uint8_t> payload, std::size_t sendCount) {
              return MakeEnhancedControlResponse(payload, sendCount, 3, true,
                                                  true);
            },
            true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  IEC61850::MmsWriteResponse response;
  const auto firstFailure =
      worker.WriteMmsControl(MakeControlCommand(), &response);
  EXPECT_EQ(firstFailure.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  const auto sendsAfterFirst = [&] {
    std::lock_guard lock(state->mutex);
    return state->sendCalls;
  }();

  const auto secondFailure =
      worker.WriteMmsControl(MakeControlCommand(), &response);
  EXPECT_EQ(secondFailure.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  {
    std::lock_guard lock(state->mutex);
    EXPECT_GT(state->sendCalls, sendsAfterFirst);
  }
  worker.Stop();
}

// 验证ctlModel=4缺少Cancel时，明确拒绝不会保留不可执行的待Cancel状态，而是锁定对象。
TEST(IEC61850MmsWorkerTest, EnhancedSboFailureWithoutCancelLocksObject) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state,
            [](std::span<const std::uint8_t> payload, std::size_t sendCount) {
              return MakeEnhancedControlResponse(payload, sendCount, 4, true,
                                                  true, false);
            },
            true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  auto select = MakeControlCommand(
      IEC61850::MmsControlOperation::SELECT_WITH_VALUE);
  IEC61850::MmsWriteResponse response;
  ASSERT_TRUE(worker.WriteMmsControl(select, &response).ok());
  EXPECT_EQ(worker.WriteMmsControl(MakeControlCommand(), &response)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(worker.WriteMmsControl(MakeControlCommand(), &response)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  auto cancel = MakeControlCommand(IEC61850::MmsControlOperation::CANCEL);
  cancel.controlValue.clear();
  EXPECT_EQ(worker.WriteMmsControl(cancel, &response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  worker.Stop();
}

// 验证增强安全CommandTermination超时后锁定对象，避免同一会话重复Oper。
TEST(IEC61850MmsWorkerTest, EnhancedOperateTimeoutLocksControlObject) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state,
            [](std::span<const std::uint8_t> payload, std::size_t sendCount) {
              return MakeEnhancedControlResponse(payload, sendCount, 3, false,
                                                  false);
            },
            true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  IEC61850::MmsWriteResponse response;
  EXPECT_EQ(worker.WriteMmsControl(MakeControlCommand(), &response)
                .error_code(),
            grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_EQ(worker.WriteMmsControl(MakeControlCommand(), &response)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  worker.Stop();
}

// 验证普通ctlModel=1的定时Oper以Write成功作为协议终态，不遗留永久pending。
TEST(IEC61850MmsWorkerTest, NormalTimedOperateCompletesAtWriteAck) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeEnhancedControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state,
            [](std::span<const std::uint8_t> payload, std::size_t sendCount) {
              return MakeEnhancedControlResponse(payload, sendCount, 1, false,
                                                  false);
            },
            true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  auto command = MakeControlCommand();
  command.operateTimestampMs = 1234;
  IEC61850::MmsWriteResponse response;
  ASSERT_TRUE(worker.WriteMmsControl(command, &response).ok());
  ASSERT_TRUE(worker.WriteMmsControl(command, &response).ok());
  worker.Stop();
}

// 验证READY会话没有在线控制能力模型时，专用控制入口不会退化为任意对象Read/Write。
TEST(IEC61850MmsWorkerTest, RejectsDedicatedControlWithoutCapabilityModel) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeMinimalPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(state, MakeControlResponse,
                                                   true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  IEC61850::MmsReadResponse selectResponse;
  EXPECT_EQ(worker.SelectMmsControl(MakeControlObject(), &selectResponse)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(selectResponse.items.empty());
  IEC61850::MmsWriteResponse writeResponse;
  EXPECT_EQ(worker.WriteMmsControl(MakeControlCommand(), &writeResponse)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(writeResponse.items.empty());
  worker.Stop();
}

// 验证计划包含FC=CO控制对象时必须协商Write服务，否则通道不能进入READY。
TEST(IEC61850MmsWorkerTest, RejectsControlPlanWithoutNegotiatedWriteService) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state, MakeControlDirectoryResponse, false,
            IEC61850Proto::NETWORK_CHANNEL_A, true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::DEGRADED;
      });
    }));
  }
  worker.Stop();
  {
    std::lock_guard lock(callbackMutex);
    EXPECT_FALSE(
        std::any_of(events.begin(), events.end(), [](const auto& event) {
          return event.state == IEC61850::ProtocolSessionState::READY;
        }));
  }
}

// 验证专用控制Write响应数量必须与单项请求严格一致，异常多项响应不会推进本地状态。
TEST(IEC61850MmsWorkerTest, RejectsUnexpectedControlWriteResultCount) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeControlPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state, MakeControlDirectoryResponseWithExtraWriteResult, true);
      });
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }
  IEC61850::MmsReadResponse selectResponse;
  ASSERT_TRUE(worker.SelectMmsControl(MakeControlObject(), &selectResponse)
                  .ok());
  IEC61850::MmsWriteResponse writeResponse;
  const auto status =
      worker.WriteMmsControl(MakeControlCommand(), &writeResponse);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(writeResponse.items.empty());
  worker.Stop();
}

// 验证READY会话中的Read和Write通过同一Worker串行队列完成MMS确认交换。
TEST(IEC61850MmsWorkerTest, ReadsAndWritesThroughReadyWorkerQueue) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeMinimalPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint& endpoint,
              IEC61850Proto::NetworkChannel channel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        state->channels.push_back(channel);
        state->endpoints.push_back(endpoint);
        return std::make_unique<ScriptedTransport>(state, MakeControlResponse,
                                                   true);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  IEC61850::MmsReadRequest readRequest;
  readRequest.variables.push_back(
      {.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC,
       .domain = "IED1LD0",
       .identifier = "LLN0$Beh$stVal"});
  IEC61850::MmsReadResponse readResponse;
  ASSERT_TRUE(worker.ReadMms(readRequest, &readResponse).ok());
  ASSERT_EQ(readResponse.items.size(), 1u);
  EXPECT_TRUE(readResponse.items.front().success);
  EXPECT_EQ(readResponse.items.front().encodedData,
            (std::vector<std::uint8_t>{0x83, 0x01, 0xff}));

  IEC61850::MmsWriteRequest writeRequest;
  IEC61850::MmsWriteRequestItem writeItem;
  writeItem.variable = readRequest.variables.front();
  ASSERT_TRUE(IEC61850::EncodeMmsDataBoolean(true, &writeItem.encodedData)
                  .ok());
  writeRequest.items.emplace_back(std::move(writeItem));
  IEC61850::MmsWriteResponse writeResponse;
  ASSERT_TRUE(worker.WriteMms(writeRequest, &writeResponse).ok());
  ASSERT_EQ(writeResponse.items.size(), 1u);
  EXPECT_TRUE(writeResponse.items.front().success);
  worker.Stop();
}

// 验证服务端未协商Write时通用WriteMms在入队前失败，且不会发送线上Write请求。
TEST(IEC61850MmsWorkerTest, RejectsGenericWriteWithoutNegotiatedService) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeMinimalPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(state, MakeControlResponse,
                                                   false);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  IEC61850::MmsWriteRequest request;
  auto& item = request.items.emplace_back();
  item.variable = {.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC,
                   .domain = "IED1LD0",
                   .identifier = "LLN0$Beh$stVal"};
  ASSERT_TRUE(IEC61850::EncodeMmsDataBoolean(true, &item.encodedData).ok());
  const auto sendsBeforeWrite = [&] {
    std::lock_guard lock(state->mutex);
    return state->sendCalls;
  }();
  IEC61850::MmsWriteResponse response;
  EXPECT_EQ(worker.WriteMms(request, &response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(response.items.empty());
  {
    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->sendCalls, sendsBeforeWrite);
  }
  worker.Stop();
}

// 验证WriteMmsControl在队列中等待超时后，工作线程取到请求时不会继续发送已取消的控制Write。
TEST(IEC61850MmsWorkerTest, CancelsQueuedControlWriteAfterWaitTimeout) {
  auto state = std::make_shared<FactoryState>();
  auto sendGate = std::make_shared<SendGate>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeControlPlan(), MakeBindings(), std::move(callbacks),
      [state, sendGate](const IEC61850::MmsTransportEndpoint&,
                        IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state, MakeControlDirectoryResponse, true,
            IEC61850Proto::NETWORK_CHANNEL_A, false,
            ScriptedTransport::ResponseDropPredicate{}, sendGate);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }

  {
    std::lock_guard lock(sendGate->mutex);
    sendGate->enabled = true;
  }

  const auto firstCommand = MakeControlCommand(
      IEC61850::MmsControlOperation::SELECT_WITH_VALUE);
  IEC61850::MmsWriteResponse firstResponse;
  grpc::Status firstStatus;
  std::thread firstRequest([&] {
    firstStatus = worker.WriteMmsControl(firstCommand, &firstResponse);
  });

  bool sendEntered = false;
  {
    std::unique_lock lock(sendGate->mutex);
    sendEntered = sendGate->condition.wait_for(
        lock, 1s, [&] { return sendGate->entered; });
  }
  if (!sendEntered) {
    {
      std::lock_guard lock(sendGate->mutex);
      sendGate->released = true;
      sendGate->condition.notify_all();
    }
    firstRequest.join();
    worker.Stop();
    ADD_FAILURE() << "控制请求未进入脚本传输发送闸门";
    return;
  }

  // 首个请求已经进入发送阶段，允许它先按已发送请求的语义超时；发送闸门仍保持阻塞。
  firstRequest.join();
  EXPECT_EQ(firstStatus.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);

  const auto queuedCommand = MakeControlCommand(
      IEC61850::MmsControlOperation::SELECT_WITH_VALUE);
  IEC61850::MmsWriteResponse queuedResponse;
  const auto beforeQueuedTimeout = [&] {
    std::lock_guard lock(state->mutex);
    return state->sendCalls;
  }();
  const auto writeStatus =
      worker.WriteMmsControl(queuedCommand, &queuedResponse);
  EXPECT_EQ(writeStatus.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  {
    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->sendCalls, beforeQueuedTimeout);
  }

  {
    std::lock_guard lock(sendGate->mutex);
    sendGate->released = true;
    sendGate->condition.notify_all();
  }

  {
    std::unique_lock lock(state->mutex);
    ASSERT_TRUE(state->condition.wait_for(lock, 1s, [&] {
      return state->sendCalls >= beforeQueuedTimeout + 1;
    }));
    EXPECT_EQ(state->sendCalls, beforeQueuedTimeout + 1);
  }
  std::this_thread::sleep_for(100ms);
  {
    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->sendCalls, beforeQueuedTimeout + 1);
  }

  worker.Stop();
}

// 验证MMS工作器按在线NameList、变量属性和DataSet成员完成真实目录核对。
TEST(IEC61850MmsWorkerTest, ValidatesOnlineDirectoryAndType) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeDirectoryPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint& endpoint,
              IEC61850Proto::NetworkChannel channel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        state->channels.push_back(channel);
        state->endpoints.push_back(endpoint);
        return std::make_unique<ScriptedTransport>(state,
                                                   MakeDirectoryResponse);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }
  worker.Stop();

  std::lock_guard lock(state->mutex);
  EXPECT_EQ(state->factoryCalls, 1u);
  EXPECT_EQ(state->connectCalls, 1u);
  EXPECT_EQ(state->closeCalls, 1u);
  EXPECT_GE(state->sendCalls, 6u);
}

// 验证唯一配置通道按Disable、Configure、Enable、GI顺序写入RCB，并通过GI报告进入READY。
TEST(IEC61850MmsWorkerTest, ActivatesRcbAndCompletesGeneralInterrogation) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeRcbPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint& endpoint,
              IEC61850Proto::NetworkChannel channel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        state->channels.push_back(channel);
        state->endpoints.push_back(endpoint);
        return std::make_unique<ScriptedTransport>(state, MakeRcbResponse,
                                                   true);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }
  worker.Stop();

  std::lock_guard lock(state->mutex);
  EXPECT_EQ(state->factoryCalls, 1u);
  EXPECT_EQ(state->connectCalls, 1u);
  EXPECT_EQ(state->closeCalls, 1u);
  EXPECT_EQ(state->sendCalls, 11u);
}

// 验证多个RCB必须分别完成配置和GI，全部RCB完成前不得发布READY。
TEST(IEC61850MmsWorkerTest, ActivatesEveryRcbBeforeReady) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  std::vector<IEC61850::MmsReportEvent> reports;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };
  callbacks.onMmsReport = [&](IEC61850::MmsReportEvent report) {
    std::lock_guard lock(callbackMutex);
    reports.emplace_back(std::move(report));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeMultiRcbPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel channel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        state->channels.push_back(channel);
        return std::make_unique<ScriptedTransport>(state, MakeRcbResponse,
                                                   true, channel);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }
  worker.Stop();

  std::lock_guard lock(callbackMutex);
  std::size_t readyEvents = 0;
  bool firstRcbReport = false;
  bool secondRcbReport = false;
  for (const auto& event : events) {
    readyEvents +=
        event.state == IEC61850::ProtocolSessionState::READY ? 1u : 0u;
  }
  for (const auto& report : reports) {
    firstRcbReport =
        firstRcbReport || report.reportRef == "IED1LD0/LLN0$BR$brcb1";
    secondRcbReport =
        secondRcbReport || report.reportRef == "IED1LD0/LLN0$UR$urcb1";
  }
  EXPECT_EQ(readyEvents, 1u);
  EXPECT_EQ(reports.size(), 2u);
  EXPECT_TRUE(firstRcbReport);
  EXPECT_TRUE(secondRcbReport);
}

// 验证活动A通道断线后B通道重新取得RCB配置权并完成完整GI。
TEST(IEC61850MmsWorkerTest, ReclaimsRcbAndGiAfterPreferredChannelDisconnects) {
  auto state = std::make_shared<FactoryState>();
  auto preferredReady = std::make_shared<std::atomic_bool>(false);
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    if (event.state == IEC61850::ProtocolSessionState::READY &&
        event.activeChannel == IEC61850Proto::NETWORK_CHANNEL_A) {
      preferredReady->store(true, std::memory_order_release);
    }
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  const auto dropBackupResponsesUntilPreferredReady =
      [preferredReady](std::span<const std::uint8_t>) {
        return !preferredReady->load(std::memory_order_acquire);
      };

  IEC61850::MmsSessionWorker worker(
      MakeRcbPlan(), MakeBindingsAB(), std::move(callbacks),
      [state, dropBackupResponsesUntilPreferredReady](
          const IEC61850::MmsTransportEndpoint&,
          IEC61850Proto::NetworkChannel channel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        ScriptedTransport::ResponseDropPredicate responseDropPredicate;
        if (channel == IEC61850Proto::NETWORK_CHANNEL_B) {
          responseDropPredicate = dropBackupResponsesUntilPreferredReady;
        }
        return std::make_unique<ScriptedTransport>(
            state, MakeRcbResponse, true, channel,
            channel == IEC61850Proto::NETWORK_CHANNEL_A ||
                channel == IEC61850Proto::NETWORK_CHANNEL_B,
            std::move(responseDropPredicate));
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 5s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY &&
               event.activeChannel == IEC61850Proto::NETWORK_CHANNEL_B;
      });
    }));
  }
  worker.Stop();

  {
    std::lock_guard lock(callbackMutex);
    bool readyOnA = false;
    bool readyOnB = false;
    for (const auto& event : events) {
      readyOnA = readyOnA ||
                 (event.state == IEC61850::ProtocolSessionState::READY &&
                  event.activeChannel == IEC61850Proto::NETWORK_CHANNEL_A);
      readyOnB = readyOnB ||
                 (event.state == IEC61850::ProtocolSessionState::READY &&
                  event.activeChannel == IEC61850Proto::NETWORK_CHANNEL_B);
    }
    EXPECT_TRUE(readyOnA);
    EXPECT_TRUE(readyOnB);
  }
  std::lock_guard lock(state->mutex);
  EXPECT_GE(state->connectCallsA, 1u);
  EXPECT_GE(state->connectCallsB, 2u);
  std::size_t bSessionCount = 0;
  bool bReconfigurationCompleted = false;
  bool bCompletedFullConfiguration = false;
  for (std::size_t index = 0; index < state->sessionChannels.size(); ++index) {
    if (state->sessionChannels[index] == IEC61850Proto::NETWORK_CHANNEL_B) {
      if (bSessionCount++ > 0 && state->sessionSendCounts[index] >= 11) {
        bReconfigurationCompleted = true;
      }
      if (state->sessionSendCounts[index] >= 11) {
        bCompletedFullConfiguration = true;
      }
    }
  }
  EXPECT_GE(bSessionCount, 2u);
  EXPECT_TRUE(bReconfigurationCompleted);
  EXPECT_TRUE(bCompletedFullConfiguration);
}

// 验证首次GI完成后，后续普通/重复报告不会无界重复发布READY状态事件。
TEST(IEC61850MmsWorkerTest, PublishesReadyOnlyOncePerSession) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeRcbPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state, MakeRcbResponseWithDuplicateGi, true);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }
  std::this_thread::sleep_for(100ms);
  worker.Stop();

  std::lock_guard lock(callbackMutex);
  const auto readyCount = std::count_if(
      events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
  EXPECT_EQ(readyCount, 1);
}

// 验证MMS会话未READY时的普通报告不会提前发布，GI报告完成后才交付数据。
TEST(IEC61850MmsWorkerTest, DropsOrdinaryReportsBeforeReady) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  std::vector<IEC61850::MmsReportEvent> reports;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };
  callbacks.onMmsReport = [&](IEC61850::MmsReportEvent report) {
    std::lock_guard lock(callbackMutex);
    reports.emplace_back(std::move(report));
    callbackCondition.notify_all();
  };

  IEC61850::MmsSessionWorker worker(
      MakeRcbPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state, MakeRcbResponseWithPreReadyOrdinaryReport, true);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      const bool ready = std::any_of(
          events.begin(), events.end(), [](const auto& event) {
            return event.state == IEC61850::ProtocolSessionState::READY;
          });
      return ready && reports.size() == 1;
    }));
    EXPECT_EQ(reports.size(), 1u);
    ASSERT_FALSE(reports.empty());
    EXPECT_TRUE(reports.front().generalInterrogation);
  }
  worker.Stop();
}

// 验证GI报告ConfRev不匹配时不会发布报告，也不会把会话推进到READY。
TEST(IEC61850MmsWorkerTest, DropsInvalidGeneralInterrogationBeforeReady) {
  auto state = std::make_shared<FactoryState>();
  std::mutex callbackMutex;
  std::vector<IEC61850::MmsConnectionEvent> events;
  std::vector<IEC61850::MmsReportEvent> reports;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
  };
  callbacks.onMmsReport = [&](IEC61850::MmsReportEvent report) {
    std::lock_guard lock(callbackMutex);
    reports.emplace_back(std::move(report));
  };

  IEC61850::MmsSessionWorker worker(
      MakeRcbPlan(), MakeBindings(), std::move(callbacks),
      [state](const IEC61850::MmsTransportEndpoint&,
              IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(
            state, MakeRcbResponseWithInvalidConfRevGi, true);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(state->mutex);
    ASSERT_TRUE(state->condition.wait_for(lock, 2s, [&] {
      return state->sendCalls >= 11;
    }));
  }
  std::this_thread::sleep_for(100ms);
  {
    std::lock_guard lock(callbackMutex);
    EXPECT_TRUE(std::none_of(
        events.begin(), events.end(), [](const auto& event) {
          return event.state == IEC61850::ProtocolSessionState::READY;
        }));
    EXPECT_TRUE(reports.empty());
  }
  worker.Stop();
}

// 验证控制确认交换期间交错到达的InformationReport会在当前会话继续交付。
TEST(IEC61850MmsWorkerTest, DrainsReportsQueuedDuringControlExchange) {
  auto state = std::make_shared<FactoryState>();
  auto giConfigured = std::make_shared<bool>(false);
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<IEC61850::MmsConnectionEvent> events;
  std::vector<IEC61850::MmsReportEvent> reports;

  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(callbackMutex);
    events.emplace_back(std::move(event));
    callbackCondition.notify_all();
  };
  callbacks.onMmsReport = [&](IEC61850::MmsReportEvent report) {
    std::lock_guard lock(callbackMutex);
    reports.emplace_back(std::move(report));
    callbackCondition.notify_all();
  };

  auto responseBuilder = [giConfigured](std::span<const std::uint8_t> payload,
                                        std::size_t sendCount) {
    IEC61850::IsoSessionPduView sessionPdu;
    if (!IEC61850::DecodeIsoSessionPdu(payload, &sessionPdu).ok() ||
        sessionPdu.type != IEC61850::IsoSessionPduType::DATA) {
      return ScriptedResponses{};
    }
    std::span<const std::uint8_t> mmsPdu;
    if (!IEC61850::DecodeMmsPresentationData(sessionPdu.userData, &mmsPdu)
             .ok()) {
      return ScriptedResponses{};
    }
    IEC61850::MmsConfirmedPduView request;
    if (!IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok()) {
      return ScriptedResponses{};
    }
    if (request.serviceTag == 5) {
      IEC61850::MmsWriteRequest write;
      std::uint32_t invokeId = 0;
      if (IEC61850::DecodeMmsWriteRequest(mmsPdu, &invokeId, &write).ok()) {
        *giConfigured = std::any_of(
            write.items.begin(), write.items.end(), [](const auto& item) {
              return item.variable.identifier.ends_with("$GI");
            });
      }
    }
    auto responses = MakeRcbResponse(payload, sendCount);
    if (request.serviceTag == 4 && *giConfigured) {
      responses.insert(responses.begin(), MakeOrdinaryReport());
    }
    return responses;
  };

  IEC61850::MmsSessionWorker worker(
      MakeRcbPlan(), MakeBindings(), std::move(callbacks),
      [state, responseBuilder](const IEC61850::MmsTransportEndpoint&,
                               IEC61850Proto::NetworkChannel) {
        std::lock_guard lock(state->mutex);
        ++state->factoryCalls;
        return std::make_unique<ScriptedTransport>(state, responseBuilder,
                                                    true);
      });

  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.state == IEC61850::ProtocolSessionState::READY;
      });
    }));
  }
  std::size_t initialReportCount = 0;
  {
    std::lock_guard lock(callbackMutex);
    initialReportCount = reports.size();
  }

  IEC61850::MmsReadRequest request;
  request.variables.push_back(
      {.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC,
       .domain = "IED1LD0",
       .identifier = "LLN0$BR$brcb1"});
  IEC61850::MmsReadResponse response;
  ASSERT_TRUE(worker.ReadMms(request, &response).ok());
  {
    std::unique_lock lock(callbackMutex);
    ASSERT_TRUE(callbackCondition.wait_for(lock, 2s, [&] {
      return reports.size() > initialReportCount;
    }));
  }
  worker.Stop();
}

// 验证传输工厂返回空对象时Start同步失败且不启动会话线程。
TEST(IEC61850MmsWorkerTest, RejectsNullTransportFromFactory) {
  auto factoryCalls = std::make_shared<std::size_t>(0);
  IEC61850::MmsSessionWorker worker(
      MakeMinimalPlan(), MakeBindings(), {},
      [factoryCalls](const IEC61850::MmsTransportEndpoint&,
                     IEC61850Proto::NetworkChannel) {
        ++*factoryCalls;
        return std::unique_ptr<IEC61850::MmsTransport>{};
      });

  const auto status = worker.Start();
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(*factoryCalls, 1u);
  EXPECT_TRUE(worker.Start().error_code() == grpc::StatusCode::FAILED_PRECONDITION);
}

}  // namespace
