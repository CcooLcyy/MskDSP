#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "IEC61850GoosePublisher.h"
#include "IEC61850RawProtocolStack.h"
#include "IEC61850SvPublisher.h"

namespace {

void AppendTlv(std::vector<std::uint8_t>* output, std::uint8_t tag,
               std::string_view value) {
  output->push_back(tag);
  output->push_back(static_cast<std::uint8_t>(value.size()));
  output->insert(output->end(), value.begin(), value.end());
}

void AppendTlv(std::vector<std::uint8_t>* output, std::uint8_t tag,
               const std::vector<std::uint8_t>& value) {
  output->push_back(tag);
  output->push_back(static_cast<std::uint8_t>(value.size()));
  output->insert(output->end(), value.begin(), value.end());
}

std::vector<std::uint8_t> MakeGoosePayloadFromAllData(
    const std::vector<std::uint8_t>& allData, bool wrongCount = false,
    bool duplicateState = false, bool missingTimestamp = false,
    int timestampLength = 8, int testLength = 1, int ndsComLength = 1) {
  std::vector<std::uint8_t> content;
  AppendTlv(&content, 0x80, "IED1LD0/LLN0$GO$gcb1");
  AppendTlv(&content, 0x81, std::vector<std::uint8_t>{0x00, 0x64});
  AppendTlv(&content, 0x82, "IED1LD0/LLN0$events");
  AppendTlv(&content, 0x83, "Trip");
  if (!missingTimestamp) {
    AppendTlv(&content, 0x84,
              std::vector<std::uint8_t>(static_cast<std::size_t>(
                                            std::max(timestampLength, 0)),
                                        0));
  }
  AppendTlv(&content, 0x85, std::vector<std::uint8_t>{0x01});
  if (duplicateState) {
    AppendTlv(&content, 0x85, std::vector<std::uint8_t>{0x01});
  }
  AppendTlv(&content, 0x86, std::vector<std::uint8_t>{0x00});
  AppendTlv(&content, 0x87,
            std::vector<std::uint8_t>(static_cast<std::size_t>(
                                          std::max(testLength, 0)),
                                      0));
  AppendTlv(&content, 0x88, std::vector<std::uint8_t>{0x04});
  AppendTlv(&content, 0x89,
            std::vector<std::uint8_t>(static_cast<std::size_t>(
                                          std::max(ndsComLength, 0)),
                                      0));
  AppendTlv(&content, 0x8a, std::vector<std::uint8_t>{
                                        static_cast<std::uint8_t>(wrongCount ? 2 : 1)});
  AppendTlv(&content, 0xab, allData);
  std::vector<std::uint8_t> payload = {0x10, 0x01, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00};
  payload.push_back(0x61);
  payload.push_back(static_cast<std::uint8_t>(content.size()));
  payload.insert(payload.end(), content.begin(), content.end());
  const auto length = static_cast<std::uint16_t>(payload.size());
  payload[2] = static_cast<std::uint8_t>(length >> 8);
  payload[3] = static_cast<std::uint8_t>(length & 0xff);
  return payload;
}

std::vector<std::uint8_t> MakeGoosePayload(bool wrongCount = false,
                                           bool duplicateState = false) {
  return MakeGoosePayloadFromAllData({0x83, 0x01, 0xff}, wrongCount,
                                     duplicateState);
}

IEC61850::ProtocolGooseSubscriptionPlan MakePlan() {
  IEC61850::ProtocolGooseSubscriptionPlan plan;
  plan.subscriptionId = 7;
  plan.controlRef = "IED1LD0/LLN0$GO$gcb1";
  plan.dataSetRef = "IED1LD0/LLN0$events";
  plan.goId = "Trip";
  plan.configRevision = 4;
  auto& member = plan.members.emplace_back();
  member.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  return plan;
}

IEC61850::ProtocolGooseSubscriptionPlan MakePublishPlan() {
  auto plan = MakePlan();
  auto& integer = plan.members.emplace_back();
  integer.valueType = IEC61850Proto::POINT_VALUE_TYPE_INT64;
  auto& floating = plan.members.emplace_back();
  floating.valueType = IEC61850Proto::POINT_VALUE_TYPE_DOUBLE;
  floating.encodedSize = 8;
  return plan;
}

IEC61850::ProtocolGoosePublisherPlan MakePublisherValidationPlan(
    std::uint32_t publisherId) {
  IEC61850::ProtocolGoosePublisherPlan plan;
  plan.publisherId = publisherId;
  plan.controlRef = "IED1LD0/LLN0$GO$gcb1";
  plan.dataSetRef = "IED1LD0/LLN0$events";
  plan.goId = "Trip";
  plan.configRevision = 4;
  auto& member = plan.members.emplace_back();
  member.dataRef = "IED1LD0/PTRC1.Tr.general";
  member.fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST;
  member.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  return plan;
}

IEC61850::ProtocolGooseSubscriptionPlan MakeFloatGoosePlan(
    std::uint8_t encodedSize) {
  auto plan = MakePlan();
  plan.members.clear();
  auto& member = plan.members.emplace_back();
  member.valueType = IEC61850Proto::POINT_VALUE_TYPE_DOUBLE;
  member.encodedSize = encodedSize;
  return plan;
}

void AppendTlvBytes(std::vector<std::uint8_t>* output, std::uint8_t tag,
                    std::initializer_list<std::uint8_t> value) {
  output->push_back(tag);
  output->push_back(static_cast<std::uint8_t>(value.size()));
  output->insert(output->end(), value.begin(), value.end());
}

void AppendBerLength(std::vector<std::uint8_t>* output, std::size_t length) {
  if (length < 0x80) {
    output->push_back(static_cast<std::uint8_t>(length));
    return;
  }
  std::array<std::uint8_t, sizeof(std::size_t)> bytes{};
  std::size_t count = 0;
  while (length != 0) {
    bytes[bytes.size() - ++count] = static_cast<std::uint8_t>(length);
    length >>= 8;
  }
  output->push_back(static_cast<std::uint8_t>(0x80 | count));
  output->insert(output->end(), bytes.begin() + bytes.size() - count,
                 bytes.end());
}

void AppendBerTlv(std::vector<std::uint8_t>* output, std::uint8_t tag,
                  std::span<const std::uint8_t> value) {
  output->push_back(tag);
  AppendBerLength(output, value.size());
  output->insert(output->end(), value.begin(), value.end());
}

std::vector<std::uint8_t> MakeWideSvPayload(std::size_t memberCount) {
  std::vector<std::uint8_t> sequenceData(memberCount * 4, 0);
  for (std::size_t index = 0; index < memberCount; ++index) {
    const auto value = static_cast<std::uint32_t>(index);
    const auto offset = index * 4;
    sequenceData[offset] = static_cast<std::uint8_t>(value >> 24);
    sequenceData[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    sequenceData[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    sequenceData[offset + 3] = static_cast<std::uint8_t>(value);
  }

  std::vector<std::uint8_t> asdu;
  const std::array<std::uint8_t, 5> svId = {'M', 'U', '2', '5', '6'};
  AppendBerTlv(&asdu, 0x80, svId);
  AppendBerTlv(&asdu, 0x82, std::array<std::uint8_t, 1>{0});
  AppendBerTlv(&asdu, 0x83, std::array<std::uint8_t, 1>{1});
  AppendBerTlv(&asdu, 0x84, std::array<std::uint8_t, 1>{2});
  AppendBerTlv(&asdu, 0x85, std::array<std::uint8_t, 8>{});
  AppendBerTlv(&asdu, 0x86, std::array<std::uint8_t, 1>{80});
  AppendBerTlv(&asdu, 0x87, sequenceData);

  std::vector<std::uint8_t> asduContainer;
  AppendBerTlv(&asduContainer, 0x30, asdu);
  std::vector<std::uint8_t> sequenceAsdu;
  AppendBerTlv(&sequenceAsdu, 0xa2, asduContainer);

  std::vector<std::uint8_t> outerContent;
  AppendBerTlv(&outerContent, 0x80, std::array<std::uint8_t, 1>{1});
  outerContent.insert(outerContent.end(), sequenceAsdu.begin(),
                      sequenceAsdu.end());
  std::vector<std::uint8_t> payload = {0x40, 0x01, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00};
  AppendBerTlv(&payload, 0x60, outerContent);
  const auto length = static_cast<std::uint16_t>(payload.size());
  payload[2] = static_cast<std::uint8_t>(length >> 8);
  payload[3] = static_cast<std::uint8_t>(length);
  return payload;
}

IEC61850::ProtocolSvStreamPlan MakeWideSvPlan(std::size_t memberCount) {
  IEC61850::ProtocolSvStreamPlan plan;
  plan.streamId = 256;
  plan.controlRef = "IED1LD0/LLN0$MS$smv256";
  plan.dataSetRef = "IED1LD0/LLN0$measurements256";
  plan.svId = "MU256";
  plan.configRevision = 1;
  plan.sampleRate = 80;
  plan.nofAsdu = 1;
  plan.members.reserve(memberCount);
  for (std::size_t index = 0; index < memberCount; ++index) {
    auto& member = plan.members.emplace_back();
    member.dataRef = "MU256/MSVCB1$MX$instMag" + std::to_string(index);
    member.fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_SV;
    member.valueType = IEC61850Proto::POINT_VALUE_TYPE_INT64;
    member.encoding = IEC61850::ProtocolSvMemberEncoding::SIGNED_INTEGER;
    member.encodedSize = 4;
  }
  return plan;
}

std::vector<std::uint8_t> MakeSvPayload(bool wrongSize = false,
                                        bool duplicateSampleCount = false,
                                        int declaredAsduCount = 2,
                                        bool unknownAsduField = false,
                                        bool unknownOuterField = false,
                                        bool trailingSavPdu = false,
                                        bool invalidRefrTmLength = false,
                                        bool reversedOptionalFields = false) {
  std::vector<std::uint8_t> sequence;
  AppendTlvBytes(&sequence, 0x80, {'M', 'U', '0', '1'});
  if (unknownAsduField) {
    AppendTlvBytes(&sequence, 0x81, {0x00});
  }
  AppendTlvBytes(&sequence, 0x82, {0x0a});
  if (duplicateSampleCount) {
    AppendTlvBytes(&sequence, 0x82, {0x0a});
  }
  AppendTlvBytes(&sequence, 0x83, {0x05});
  AppendTlvBytes(&sequence, 0x84, {0x02});
  const auto appendRefrTm = [&sequence, invalidRefrTmLength]() {
    if (invalidRefrTmLength) {
      AppendTlvBytes(&sequence, 0x85, {0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00});
    } else {
      AppendTlvBytes(&sequence, 0x85, {0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00});
    }
  };
  if (reversedOptionalFields) {
    AppendTlvBytes(&sequence, 0x86, {0x50});
    appendRefrTm();
  } else {
    appendRefrTm();
    AppendTlvBytes(&sequence, 0x86, {0x50});
  }
  AppendTlvBytes(&sequence, 0x87,
                 {0x00, 0x00, 0x00, 0x7b, 0x3f, 0xf8, 0x00, 0x00,
                  0x00, 0x00, 0x00, 0x00});
  std::vector<std::uint8_t> first{0x30,
                                  static_cast<std::uint8_t>(sequence.size())};
  first.insert(first.end(), sequence.begin(), sequence.end());

  sequence[sequence.size() - 12 + 3] = 0x7c;
  std::vector<std::uint8_t> second{0x30,
                                   static_cast<std::uint8_t>(sequence.size())};
  second.insert(second.end(), sequence.begin(), sequence.end());
  std::vector<std::uint8_t> asduContainer{0xa2,
                                          static_cast<std::uint8_t>(
                                              first.size() + second.size())};
  asduContainer.insert(asduContainer.end(), first.begin(), first.end());
  asduContainer.insert(asduContainer.end(), second.begin(), second.end());
  std::vector<std::uint8_t> outerContent;
  if (declaredAsduCount >= 0) {
    AppendTlvBytes(
        &outerContent, 0x80,
        {static_cast<std::uint8_t>(declaredAsduCount)});
  }
  if (unknownOuterField) {
    AppendTlvBytes(&outerContent, 0x81, {0x00});
  }
  outerContent.insert(outerContent.end(), asduContainer.begin(),
                      asduContainer.end());
  std::vector<std::uint8_t> outer{
      0x60, static_cast<std::uint8_t>(outerContent.size())};
  outer.insert(outer.end(), outerContent.begin(), outerContent.end());
  std::vector<std::uint8_t> payload = {0x40, 0x01, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00};
  payload.insert(payload.end(), outer.begin(), outer.end());
  if (trailingSavPdu) {
    AppendTlvBytes(&payload, 0x01, {0x00});
  }
  if (wrongSize) {
    payload.pop_back();
  }
  const auto length = static_cast<std::uint16_t>(payload.size());
  payload[2] = static_cast<std::uint8_t>(length >> 8);
  payload[3] = static_cast<std::uint8_t>(length & 0xff);
  return payload;
}

IEC61850::ProtocolSvStreamPlan MakeSvPlan() {
  IEC61850::ProtocolSvStreamPlan plan;
  plan.streamId = 4;
  plan.svId = "MU01";
  plan.configRevision = 5;
  plan.sampleRate = 80;
  plan.nofAsdu = 2;
  auto& integer = plan.members.emplace_back();
  integer.valueType = IEC61850Proto::POINT_VALUE_TYPE_INT64;
  integer.encoding = IEC61850::ProtocolSvMemberEncoding::SIGNED_INTEGER;
  integer.encodedSize = 4;
  auto& floating = plan.members.emplace_back();
  floating.valueType = IEC61850Proto::POINT_VALUE_TYPE_DOUBLE;
  floating.encoding = IEC61850::ProtocolSvMemberEncoding::FLOATING_POINT;
  floating.encodedSize = 8;
  return plan;
}

IEC61850::SvPublisherConfig MakeSvPublisherConfig() {
  IEC61850::SvPublisherConfig config;
  config.endpoint.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  config.endpoint.interfaceName = "lo";
  config.endpoint.destinationMac = "01:0c:cd:04:00:01";
  config.endpoint.appId = 0x4001;
  config.svId = "MU01";
  config.configRevision = 5;
  config.sampleRate = 80;
  config.nofAsdu = 2;
  auto& integer = config.members.emplace_back();
  integer.dataRef = "MU01/MSVCB1$MX$instMag";
  integer.fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_SV;
  integer.valueType = IEC61850Proto::POINT_VALUE_TYPE_INT64;
  integer.encoding = IEC61850::ProtocolSvMemberEncoding::SIGNED_INTEGER;
  integer.encodedSize = 4;
  auto& floating = config.members.emplace_back();
  floating.dataRef = "MU01/MSVCB1$MX$instMag.f";
  floating.fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_SV;
  floating.valueType = IEC61850Proto::POINT_VALUE_TYPE_DOUBLE;
  floating.encoding = IEC61850::ProtocolSvMemberEncoding::FLOATING_POINT;
  floating.encodedSize = 8;
  return config;
}

IEC61850::SvPublishRequest MakeSvPublishRequest(
    const IEC61850::ProtocolSvStreamPlan& plan,
    std::span<const std::uint16_t> sampleCounts,
    std::span<const IEC61850::ProtocolRealtimeValue> values) {
  IEC61850::SvPublishRequest request;
  request.svId = plan.svId;
  request.configRevision = plan.configRevision;
  request.sampleRate = plan.sampleRate;
  request.sampleSynchronization = 2;
  request.members = plan.members;
  request.sampleCounts = sampleCounts;
  request.values = values;
  request.referenceTimeMs = 1'700'000'000'123;
  return request;
}

void ConfigureLiveGoosePlan(IEC61850::ProtocolGooseSubscriptionPlan* plan,
                            std::string_view interfaceName,
                            bool vlanTagged = false) {
  ASSERT_NE(plan, nullptr);
  ASSERT_FALSE(plan->members.empty());
  plan->members[0].dataRef = "IED1LD0/LLN0$ST$stVal";
  plan->members[0].fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST;
  auto& endpoint = plan->endpoints.emplace_back();
  endpoint.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  endpoint.interfaceName = interfaceName;
  endpoint.destinationMac = "01:0c:cd:01:00:01";
  endpoint.appId = 0x1001;
  endpoint.vlanTagged = vlanTagged;
  endpoint.vlanId = vlanTagged ? 7 : 0;
  endpoint.vlanPriority = vlanTagged ? 4 : 0;
}

void ConfigureLiveSvPlan(IEC61850::ProtocolSvStreamPlan* plan,
                         std::string_view interfaceName,
                         bool vlanTagged = false) {
  ASSERT_NE(plan, nullptr);
  ASSERT_EQ(plan->members.size(), 2u);
  plan->controlRef = "IED1LD0/LLN0$MS$smv1";
  plan->dataSetRef = "IED1LD0/LLN0$measurements";
  plan->sampleRate = 80;
  plan->members[0].dataRef = "MU01/MSVCB1$MX$instMag";
  plan->members[0].fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_SV;
  plan->members[1].dataRef = "MU01/MSVCB1$MX$instMag.f";
  plan->members[1].fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_SV;
  auto& endpoint = plan->endpoints.emplace_back();
  endpoint.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  endpoint.interfaceName = interfaceName;
  endpoint.destinationMac = "01:0c:cd:04:00:01";
  endpoint.appId = 0x4001;
  endpoint.vlanTagged = vlanTagged;
  endpoint.vlanId = vlanTagged ? 7 : 0;
  endpoint.vlanPriority = vlanTagged ? 4 : 0;
}

std::vector<std::uint8_t> MakeLiveEthernetFrame(
    const IEC61850::RawEthernetFilter& filter,
    std::span<const std::uint8_t> payload,
    const std::array<std::uint8_t, 6>& sourceMac) {
  const std::size_t headerSize = filter.vlanTagged ? 18 : 14;
  std::vector<std::uint8_t> frame(headerSize + payload.size(), 0);
  std::copy(filter.destinationMac.begin(), filter.destinationMac.end(),
            frame.begin());
  std::copy(sourceMac.begin(), sourceMac.end(), frame.begin() + 6);
  if (filter.vlanTagged) {
    frame[12] = 0x81;
    frame[13] = 0x00;
    frame[14] = static_cast<std::uint8_t>(
        (filter.vlanPriority.value_or(0) << 5) | (filter.vlanId >> 8));
    frame[15] = static_cast<std::uint8_t>(filter.vlanId);
    frame[16] = static_cast<std::uint8_t>(filter.etherType >> 8);
    frame[17] = static_cast<std::uint8_t>(filter.etherType);
  } else {
    frame[12] = static_cast<std::uint8_t>(filter.etherType >> 8);
    frame[13] = static_cast<std::uint8_t>(filter.etherType);
  }
  std::copy(payload.begin(), payload.end(), frame.begin() + headerSize);
  return frame;
}

IEC61850::ProtocolIedPlan MakePartialStartPlan() {
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("partial-start");
  plan.config.set_enable_mms(true);
  plan.config.set_enable_goose(true);

  auto& binding = plan.networkBindings.emplace_back();
  binding.channel.set_channel(IEC61850Proto::NETWORK_CHANNEL_A);
  binding.channel.set_enabled(true);
  binding.channel.set_interface_name("lo");
  binding.channel.set_remote_ip("127.0.0.1");
  binding.channel.set_remote_port(1);

  auto& subscription = plan.gooseSubscriptions.emplace_back();
  subscription.subscriptionId = 1;
  subscription.controlRef = "IED1LD0/LLN0$GO$gcb1";
  subscription.dataSetRef = "IED1LD0/LLN0$events";
  subscription.goId = "Trip";
  subscription.configRevision = 1;
  auto& member = subscription.members.emplace_back();
  member.dataRef = "IED1LD0/LLN0$ST$stVal";
  member.fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST;
  member.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  auto& endpoint = subscription.endpoints.emplace_back();
  endpoint.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  endpoint.interfaceName = "iec61850-nonexistent0";
  endpoint.destinationMac = "01:0c:cd:01:00:01";
  endpoint.appId = 1;
  return plan;
}

IEC61850::ProtocolGooseSubscriptionPlan MakeGooseValidationSubscription(
    std::uint32_t subscriptionId) {
  IEC61850::ProtocolGooseSubscriptionPlan subscription;
  subscription.subscriptionId = subscriptionId;
  subscription.controlRef = "IED1LD0/LLN0$GO$gcb1";
  subscription.dataSetRef = "IED1LD0/LLN0$events";
  subscription.goId = "Trip";
  subscription.configRevision = 1;
  auto& member = subscription.members.emplace_back();
  member.dataRef = "IED1LD0/LLN0$ST$stVal";
  member.fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST;
  member.valueType = IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  return subscription;
}

IEC61850::ProtocolGooseSubscriptionPlan MakeGooseLoopbackSubscription(
    std::uint32_t subscriptionId) {
  auto subscription = MakeGooseValidationSubscription(subscriptionId);
  auto& endpoint = subscription.endpoints.emplace_back();
  endpoint.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  endpoint.interfaceName = "lo";
  endpoint.destinationMac = "01:0c:cd:01:00:01";
  endpoint.appId = 0x1001;
  return subscription;
}

IEC61850::ProtocolGoosePublisherPlan MakeGooseLoopbackPublisher(
    std::uint32_t publisherId, std::string_view destinationMac,
    std::uint16_t appId) {
  auto publisher = MakePublisherValidationPlan(publisherId);
  auto& endpoint = publisher.endpoints.emplace_back();
  endpoint.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  endpoint.interfaceName = "lo";
  endpoint.destinationMac = destinationMac;
  endpoint.appId = appId;
  return publisher;
}

IEC61850::ProtocolGooseSubscriptionPlan MakeGooseDecodePlan(
    const IEC61850::ProtocolGoosePublisherPlan& publisher) {
  IEC61850::ProtocolGooseSubscriptionPlan plan;
  plan.subscriptionId = publisher.publisherId;
  plan.publisherIed = publisher.publisherIed;
  plan.controlRef = publisher.controlRef;
  plan.dataSetRef = publisher.dataSetRef;
  plan.goId = publisher.goId;
  plan.configRevision = publisher.configRevision;
  plan.members = publisher.members;
  plan.endpoints = publisher.endpoints;
  return plan;
}

grpc::Status PollRawEthernetFrame(
    IEC61850::RawEthernetSocket* socket, std::span<std::uint8_t> storage,
    IEC61850::RawEthernetFrameView* view,
    std::chrono::milliseconds timeout, bool* received) {
  if (socket == nullptr || view == nullptr || received == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "GOOSE测试接收参数不能为空");
  }
  *received = false;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto status = socket->Receive(storage, view);
    if (!status.ok()) {
      return status;
    }
    if (!view->payload.empty()) {
      *received = true;
      return grpc::Status::OK;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return grpc::Status::OK;
}

IEC61850::ProtocolSvStreamPlan MakeSvValidationStream(
    std::uint32_t streamId) {
  IEC61850::ProtocolSvStreamPlan stream;
  stream.streamId = streamId;
  stream.controlRef = "IED1LD0/LLN0$MS$smv1";
  stream.dataSetRef = "IED1LD0/LLN0$measurements";
  stream.svId = "MU01";
  stream.configRevision = 1;
  stream.sampleRate = 80;
  stream.nofAsdu = 1;
  auto& member = stream.members.emplace_back();
  member.dataRef = "MU01/MSVCB1$MX$instMag";
  member.fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_SV;
  member.valueType = IEC61850Proto::POINT_VALUE_TYPE_INT64;
  member.encoding = IEC61850::ProtocolSvMemberEncoding::SIGNED_INTEGER;
  member.encodedSize = 4;
  return stream;
}

}  // namespace

// 验证GOOSE BER基础字段和定长布尔成员能够被解码为协议回调视图。
TEST(IEC61850RawProtocolStackTest, DecodesGoosePayload) {
  auto payload = MakeGoosePayload();
  const auto plan = MakePlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(1);
  IEC61850::ProtocolGooseFrameView frame;

  ASSERT_TRUE(IEC61850::DecodeGoosePayload(payload, plan,
                                            IEC61850Proto::NETWORK_CHANNEL_A,
                                            values, &frame));
  EXPECT_EQ(frame.subscriptionId, 7u);
  EXPECT_EQ(frame.appId, 0x1001u);
  EXPECT_EQ(frame.stateNumber, 1u);
  EXPECT_EQ(frame.timeAllowedToLiveMs, 100u);
  EXPECT_GT(frame.receiveTimestampNs, 0);
  EXPECT_EQ(values.front().timestampNs, frame.receiveTimestampNs);
  EXPECT_TRUE(values.front().value.booleanValue);
}

// 验证GOOSE数据集成员数量不一致时整帧被拒绝。
TEST(IEC61850RawProtocolStackTest, RejectsGooseMemberCountMismatch) {
  const auto payload = MakeGoosePayload(true);
  const auto plan = MakePlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(1);
  IEC61850::ProtocolGooseFrameView frame;

  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));
}

// 验证GOOSE APPID载荷长度与实际接收长度不一致时整帧被拒绝。
TEST(IEC61850RawProtocolStackTest, RejectsGoosePayloadLengthMismatch) {
  auto payload = MakeGoosePayload();
  payload.push_back(0);
  const auto plan = MakePlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(1);
  IEC61850::ProtocolGooseFrameView frame;

  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));
}

// 验证GOOSE应用PDU后附加未知TLV时不会被部分解析后交付。
TEST(IEC61850RawProtocolStackTest, RejectsTrailingGoosePduField) {
  auto payload = MakeGoosePayload();
  payload.push_back(0x01);
  payload.push_back(0x00);
  const auto length = static_cast<std::uint16_t>(payload.size());
  payload[2] = static_cast<std::uint8_t>(length >> 8);
  payload[3] = static_cast<std::uint8_t>(length);
  const auto plan = MakePlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(1);
  IEC61850::ProtocolGooseFrameView frame;

  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));
}

// 验证GOOSE必需字段重复时不会使用第一个字段继续交付。
TEST(IEC61850RawProtocolStackTest, RejectsDuplicateGooseMandatoryField) {
  const auto payload = MakeGoosePayload(false, true);
  const auto plan = MakePlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(1);
  IEC61850::ProtocolGooseFrameView frame;

  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));
}

// 验证GOOSE缺少必需的UtcTime字段时不会交付整帧。
TEST(IEC61850RawProtocolStackTest, RejectsMissingGooseUtcTime) {
  const auto payload = MakeGoosePayloadFromAllData(
      {0x83, 0x01, 0xff}, false, false, true);
  const auto plan = MakePlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(1);
  IEC61850::ProtocolGooseFrameView frame;

  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));
}

// 验证GOOSE UtcTime必须严格为8字节。
TEST(IEC61850RawProtocolStackTest, RejectsInvalidGooseUtcTimeLength) {
  const auto payload = MakeGoosePayloadFromAllData(
      {0x83, 0x01, 0xff}, false, false, false, 7);
  const auto plan = MakePlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(1);
  IEC61850::ProtocolGooseFrameView frame;

  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));
}

// 验证GOOSE头部的test和ndsCom字段必须是单字节BER BOOLEAN。
TEST(IEC61850RawProtocolStackTest, RejectsNonCanonicalGooseHeaderBoolean) {
  const auto plan = MakePlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(1);
  IEC61850::ProtocolGooseFrameView frame;

  const auto invalidTest = MakeGoosePayloadFromAllData(
      {0x83, 0x01, 0xff}, false, false, false, 8, 2, 1);
  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      invalidTest, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));

  const auto emptyTest = MakeGoosePayloadFromAllData(
      {0x83, 0x01, 0xff}, false, false, false, 8, 0, 1);
  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      emptyTest, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));

  const auto invalidNdsCom = MakeGoosePayloadFromAllData(
      {0x83, 0x01, 0xff}, false, false, false, 8, 1, 2);
  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      invalidNdsCom, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));

  const auto emptyNdsCom = MakeGoosePayloadFromAllData(
      {0x83, 0x01, 0xff}, false, false, false, 8, 1, 0);
  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      emptyNdsCom, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));
}

// 验证GOOSE DataSet中的BOOLEAN成员也必须是单字节BER BOOLEAN。
TEST(IEC61850RawProtocolStackTest, RejectsNonCanonicalGooseDataBoolean) {
  const auto payload = MakeGoosePayloadFromAllData({0x83, 0x02, 0x00, 0xff});
  const auto plan = MakePlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(1);
  IEC61850::ProtocolGooseFrameView frame;

  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));
}

// 验证GOOSE编码器生成的BOOL、整数和浮点成员能够被接收解码器回放。
TEST(IEC61850RawProtocolStackTest, EncodesAndDecodesGoosePayload) {
  const auto plan = MakePublishPlan();
  std::array<IEC61850::ProtocolRealtimeValue, 3> values{};
  values[0].valueType = IEC61850::ProtocolRealtimeValueType::BOOLEAN;
  values[0].value.booleanValue = true;
  values[1].valueType = IEC61850::ProtocolRealtimeValueType::INTEGER;
  values[1].value.integerValue = -23;
  values[2].valueType = IEC61850::ProtocolRealtimeValueType::FLOATING;
  values[2].value.floatingValue = 1.25;
  IEC61850::GoosePublishRequest request;
  request.gocbRef = plan.controlRef;
  request.dataSetRef = plan.dataSetRef;
  request.goId = plan.goId;
  request.timeAllowedToLiveMs = 500;
  request.configRevision = plan.configRevision;
  request.members = plan.members;
  request.values = values;
  std::array<std::uint8_t, 2048> payload{};
  std::size_t payloadSize = 0;

  ASSERT_TRUE(IEC61850::EncodeGoosePayload(
      request, 2, 3, 0x1001, payload, &payloadSize));
  ASSERT_GT(payloadSize, 8u);
  std::vector<IEC61850::ProtocolRealtimeValue> decoded(values.size());
  IEC61850::ProtocolGooseFrameView frame;
  ASSERT_TRUE(IEC61850::DecodeGoosePayload(
      std::span<const std::uint8_t>(payload.data(), payloadSize), plan,
      IEC61850Proto::NETWORK_CHANNEL_A, decoded, &frame));
  EXPECT_EQ(frame.stateNumber, 2u);
  EXPECT_EQ(frame.sequenceNumber, 3u);
  EXPECT_EQ(frame.timeAllowedToLiveMs, 500u);
  EXPECT_TRUE(decoded[0].value.booleanValue);
  EXPECT_EQ(decoded[1].value.integerValue, -23);
  EXPECT_DOUBLE_EQ(decoded[2].value.floatingValue, 1.25);
}

// 验证GOOSE Quality成员按13位BIT STRING标准编码，并可无损回放解码。
TEST(IEC61850RawProtocolStackTest, EncodesAndDecodesGooseQualityBitString) {
  auto plan = MakePlan();
  plan.members[0].valueType = IEC61850Proto::POINT_VALUE_TYPE_INT64;
  plan.members[0].qualityValue = true;
  std::array<IEC61850::ProtocolRealtimeValue, 1> values{};
  values[0].valueType = IEC61850::ProtocolRealtimeValueType::INTEGER;
  values[0].value.integerValue = 0x1234;

  IEC61850::GoosePublishRequest request;
  request.gocbRef = plan.controlRef;
  request.dataSetRef = plan.dataSetRef;
  request.goId = plan.goId;
  request.timeAllowedToLiveMs = 500;
  request.configRevision = plan.configRevision;
  request.members = plan.members;
  request.values = values;
  std::array<std::uint8_t, 2048> payload{};
  std::size_t payloadSize = 0;

  ASSERT_TRUE(IEC61850::EncodeGoosePayload(
      request, 1, 0, 0x1001, payload, &payloadSize));
  const std::array<std::uint8_t, 5> expectedQuality{
      0x84, 0x03, 0x03, 0x91, 0xa0};
  EXPECT_NE(std::search(payload.begin(), payload.begin() + payloadSize,
                        expectedQuality.begin(), expectedQuality.end()),
            payload.begin() + payloadSize);

  std::vector<IEC61850::ProtocolRealtimeValue> decoded(1);
  IEC61850::ProtocolGooseFrameView frame;
  ASSERT_TRUE(IEC61850::DecodeGoosePayload(
      std::span<const std::uint8_t>(payload.data(), payloadSize), plan,
      IEC61850Proto::NETWORK_CHANNEL_A, decoded, &frame));
  EXPECT_EQ(decoded[0].valueType,
            IEC61850::ProtocolRealtimeValueType::INTEGER);
  EXPECT_EQ(decoded[0].value.integerValue, 0x1234);
  EXPECT_EQ(decoded[0].qualityBits, 0x1234u);
}

// 验证GOOSE Quality的BIT STRING长度、未使用位和填充位不符合标准时拒绝整帧。
TEST(IEC61850RawProtocolStackTest, RejectsNonCanonicalGooseQualityBitString) {
  auto plan = MakePlan();
  plan.members[0].valueType = IEC61850Proto::POINT_VALUE_TYPE_INT64;
  plan.members[0].qualityValue = true;
  std::vector<IEC61850::ProtocolRealtimeValue> values(1);
  IEC61850::ProtocolGooseFrameView frame;

  const auto wrongLength = MakeGoosePayloadFromAllData(
      {0x84, 0x02, 0x03, 0x91});
  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      wrongLength, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));

  const auto wrongUnusedBits = MakeGoosePayloadFromAllData(
      {0x84, 0x03, 0x02, 0x91, 0xa0});
  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      wrongUnusedBits, plan, IEC61850Proto::NETWORK_CHANNEL_A, values,
      &frame));

  const auto nonZeroPadding = MakeGoosePayloadFromAllData(
      {0x84, 0x03, 0x03, 0x91, 0xa1});
  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      nonZeroPadding, plan, IEC61850Proto::NETWORK_CHANNEL_A, values,
      &frame));
}

// 验证GOOSE标准FLOAT32值包含format-width并按IEEE-754大端值解码。
TEST(IEC61850RawProtocolStackTest, DecodesStandardGooseFloat32) {
  const auto plan = MakeFloatGoosePlan(4);
  const auto payload = MakeGoosePayloadFromAllData(
      {0x87, 0x05, 0x08, 0x3f, 0xc0, 0x00, 0x00});
  std::array<IEC61850::ProtocolRealtimeValue, 1> values{};
  IEC61850::ProtocolGooseFrameView frame;

  ASSERT_TRUE(IEC61850::DecodeGoosePayload(payload, plan,
                                            IEC61850Proto::NETWORK_CHANNEL_A,
                                            values, &frame));
  EXPECT_DOUBLE_EQ(values[0].value.floatingValue, 1.5);
}

// 验证GOOSE标准FLOAT64值包含format-width并按IEEE-754大端值解码。
TEST(IEC61850RawProtocolStackTest, DecodesStandardGooseFloat64) {
  const auto plan = MakeFloatGoosePlan(8);
  const auto payload = MakeGoosePayloadFromAllData(
      {0x87, 0x09, 0x0b, 0x3f, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  std::array<IEC61850::ProtocolRealtimeValue, 1> values{};
  IEC61850::ProtocolGooseFrameView frame;

  ASSERT_TRUE(IEC61850::DecodeGoosePayload(payload, plan,
                                            IEC61850Proto::NETWORK_CHANNEL_A,
                                            values, &frame));
  EXPECT_DOUBLE_EQ(values[0].value.floatingValue, 1.25);
}

// 验证GOOSE浮点值缺少format-width时不会按裸IEEE字节误解码。
TEST(IEC61850RawProtocolStackTest, RejectsGooseFloatWithoutFormatWidth) {
  const auto plan = MakeFloatGoosePlan(4);
  const auto payload = MakeGoosePayloadFromAllData(
      {0x87, 0x04, 0x3f, 0xc0, 0x00, 0x00});
  std::array<IEC61850::ProtocolRealtimeValue, 1> values{};
  IEC61850::ProtocolGooseFrameView frame;

  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));
}

// 验证GOOSE浮点值的线宽度与启动计划不一致时拒绝整帧。
TEST(IEC61850RawProtocolStackTest, RejectsGooseFloatWidthMismatch) {
  const auto plan = MakeFloatGoosePlan(4);
  const auto payload = MakeGoosePayloadFromAllData(
      {0x87, 0x09, 0x08, 0x3f, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  std::array<IEC61850::ProtocolRealtimeValue, 1> values{};
  IEC61850::ProtocolGooseFrameView frame;

  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));
}

// 验证GOOSE FLOAT64不能使用FLOAT32的format-width。
TEST(IEC61850RawProtocolStackTest, RejectsGooseFloat64WithFloat32FormatWidth) {
  const auto plan = MakeFloatGoosePlan(8);
  const auto payload = MakeGoosePayloadFromAllData(
      {0x87, 0x09, 0x08, 0x3f, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  std::array<IEC61850::ProtocolRealtimeValue, 1> values{};
  IEC61850::ProtocolGooseFrameView frame;

  EXPECT_FALSE(IEC61850::DecodeGoosePayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, &frame));
}

// 验证GOOSE编码器输出FLOAT32时写入format-width和标准4字节载荷。
TEST(IEC61850RawProtocolStackTest, EncodesStandardGooseFloat32) {
  const auto plan = MakeFloatGoosePlan(4);
  std::array<IEC61850::ProtocolRealtimeValue, 1> values{};
  values[0].valueType = IEC61850::ProtocolRealtimeValueType::FLOATING;
  values[0].value.floatingValue = 1.5;
  IEC61850::GoosePublishRequest request;
  request.gocbRef = plan.controlRef;
  request.dataSetRef = plan.dataSetRef;
  request.goId = plan.goId;
  request.timeAllowedToLiveMs = 500;
  request.configRevision = plan.configRevision;
  request.members = plan.members;
  request.values = values;
  std::array<std::uint8_t, 2048> payload{};
  std::size_t payloadSize = 0;

  ASSERT_TRUE(IEC61850::EncodeGoosePayload(
      request, 1, 0, 0x1001, payload, &payloadSize));
  const std::array<std::uint8_t, 7> expected{
      0x87, 0x05, 0x08, 0x3f, 0xc0, 0x00, 0x00};
  EXPECT_NE(std::search(payload.begin(), payload.begin() + payloadSize,
                        expected.begin(), expected.end()),
            payload.begin() + payloadSize);
}

// 验证GOOSE编码器输出FLOAT64时写入format-width和标准8字节载荷。
TEST(IEC61850RawProtocolStackTest, EncodesStandardGooseFloat64) {
  const auto plan = MakeFloatGoosePlan(8);
  std::array<IEC61850::ProtocolRealtimeValue, 1> values{};
  values[0].valueType = IEC61850::ProtocolRealtimeValueType::FLOATING;
  values[0].value.floatingValue = 1.25;
  IEC61850::GoosePublishRequest request;
  request.gocbRef = plan.controlRef;
  request.dataSetRef = plan.dataSetRef;
  request.goId = plan.goId;
  request.timeAllowedToLiveMs = 500;
  request.configRevision = plan.configRevision;
  request.members = plan.members;
  request.values = values;
  std::array<std::uint8_t, 2048> payload{};
  std::size_t payloadSize = 0;

  ASSERT_TRUE(IEC61850::EncodeGoosePayload(
      request, 1, 0, 0x1001, payload, &payloadSize));
  const std::array<std::uint8_t, 11> expected{
      0x87, 0x09, 0x0b, 0x3f, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_NE(std::search(payload.begin(), payload.begin() + payloadSize,
                        expected.begin(), expected.end()),
            payload.begin() + payloadSize);
}

// 验证SV一个报文内的多个ASDU能够按计划解析不同采样宽度。
TEST(IEC61850RawProtocolStackTest, DecodesMultipleSvAsdusAndWidths) {
  const auto payload = MakeSvPayload();
  const auto plan = MakeSvPlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(4);
  std::vector<IEC61850::ProtocolSvFrameView> frames(2);
  std::size_t frameCount = 0;

  ASSERT_TRUE(IEC61850::DecodeSvPayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, frames,
      &frameCount));
  ASSERT_EQ(frameCount, 2u);
  ASSERT_EQ(frames[0].values.size(), 2u);
  EXPECT_EQ(frames[0].sampleCount, 10u);
  EXPECT_EQ(frames[0].values[0].value.integerValue, 123);
  EXPECT_DOUBLE_EQ(frames[0].values[1].value.floatingValue, 1.5);
  EXPECT_EQ(frames[1].asduIndex, 1u);
  EXPECT_EQ(frames[1].values[0].value.integerValue, 124);
}

// 验证SV发布编码保留svID、ConfRev、采样率、ASDU顺序和smpCnt，并可由接收解码器回放。
TEST(IEC61850RawProtocolStackTest, EncodesAndDecodesSvPayload) {
  const auto plan = MakeSvPlan();
  const std::array<std::uint16_t, 2> sampleCounts{10, 11};
  std::array<IEC61850::ProtocolRealtimeValue, 4> values{};
  values[0].valueType = IEC61850::ProtocolRealtimeValueType::INTEGER;
  values[0].value.integerValue = -123;
  values[1].valueType = IEC61850::ProtocolRealtimeValueType::FLOATING;
  values[1].value.floatingValue = 1.5;
  values[2].valueType = IEC61850::ProtocolRealtimeValueType::INTEGER;
  values[2].value.integerValue = 456;
  values[3].valueType = IEC61850::ProtocolRealtimeValueType::FLOATING;
  values[3].value.floatingValue = -2.25;
  const auto request = MakeSvPublishRequest(plan, sampleCounts, values);
  std::array<std::uint8_t, 2048> payload{};
  std::size_t payloadSize = 0;

  ASSERT_TRUE(IEC61850::EncodeSvPayload(request, 0x4001, payload,
                                        &payloadSize));
  ASSERT_GT(payloadSize, 8u);
  std::vector<IEC61850::ProtocolRealtimeValue> decoded(values.size());
  std::vector<IEC61850::ProtocolSvFrameView> frames(2);
  std::size_t frameCount = 0;
  ASSERT_TRUE(IEC61850::DecodeSvPayload(
      std::span<const std::uint8_t>(payload.data(), payloadSize), plan,
      IEC61850Proto::NETWORK_CHANNEL_A, decoded, frames, &frameCount));
  ASSERT_EQ(frameCount, 2u);
  EXPECT_EQ(frames[0].sampleCount, 10u);
  EXPECT_EQ(frames[1].sampleCount, 11u);
  EXPECT_EQ(decoded[0].value.integerValue, -123);
  EXPECT_DOUBLE_EQ(decoded[1].value.floatingValue, 1.5);
  EXPECT_EQ(decoded[2].value.integerValue, 456);
  EXPECT_DOUBLE_EQ(decoded[3].value.floatingValue, -2.25);
}

// 验证SV发布编码器在成员数量、采样计数数量或输出缓冲不足时原子拒绝。
TEST(IEC61850RawProtocolStackTest, RejectsInvalidSvPublishRequest) {
  const auto plan = MakeSvPlan();
  const std::array<std::uint16_t, 1> shortCounts{10};
  std::array<IEC61850::ProtocolRealtimeValue, 4> values{};
  for (auto& value : values) {
    value.valueType = IEC61850::ProtocolRealtimeValueType::INTEGER;
  }
  auto request = MakeSvPublishRequest(plan, shortCounts, values);
  std::array<std::uint8_t, 2048> payload{};
  std::size_t payloadSize = 99;
  EXPECT_FALSE(IEC61850::EncodeSvPayload(request, 0x4001, payload,
                                          &payloadSize));
  EXPECT_EQ(payloadSize, 0u);

  request.sampleCounts = std::array<std::uint16_t, 2>{10, 11};
  request.values = std::span<const IEC61850::ProtocolRealtimeValue>(
      values.data(), values.size() - 1);
  payloadSize = 99;
  EXPECT_FALSE(IEC61850::EncodeSvPayload(request, 0x4001, payload,
                                          &payloadSize));
  EXPECT_EQ(payloadSize, 0u);

  request.values = values;
  std::array<std::uint8_t, 8> tinyOutput{};
  payloadSize = 99;
  EXPECT_FALSE(IEC61850::EncodeSvPayload(request, 0x4001, tinyOutput,
                                          &payloadSize));
  EXPECT_EQ(payloadSize, 0u);
}

// 验证SV发布端在lo二层端点发送后可被抓包解码，并按ASDU顺序推进smpCnt。
TEST(IEC61850RawProtocolStackTest, PublishesSvOnLoopbackAndAdvancesSampleCount) {
  auto config = MakeSvPublisherConfig();
  IEC61850::SvPublisher publisher(config);
  const auto opened = publisher.Open();
  if (!opened.ok()) {
    if (opened.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
        opened.error_code() == grpc::StatusCode::UNAVAILABLE) {
      GTEST_SKIP() << "当前环境不允许打开lo上的SV发布套接字: "
                   << opened.error_message();
    }
    FAIL() << "SV发布端打开失败: " << opened.error_message();
  }

  IEC61850::RawEthernetFilter filter;
  filter.destinationMac = {0x01, 0x0c, 0xcd, 0x04, 0x00, 0x01};
  filter.etherType = 0x88ba;
  filter.appId = 0x4001;
  IEC61850::RawEthernetSocket capture;
  const auto captureStatus = capture.Open("lo", filter);
  if (!captureStatus.ok()) {
    publisher.Close();
    if (captureStatus.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
        captureStatus.error_code() == grpc::StatusCode::UNAVAILABLE) {
      GTEST_SKIP() << "当前环境不允许打开lo上的SV抓包套接字: "
                   << captureStatus.error_message();
    }
    FAIL() << "SV抓包套接字打开失败: " << captureStatus.error_message();
  }

  std::array<IEC61850::ProtocolRealtimeValue, 4> values{};
  values[0].valueType = IEC61850::ProtocolRealtimeValueType::INTEGER;
  values[0].value.integerValue = -123;
  values[1].valueType = IEC61850::ProtocolRealtimeValueType::FLOATING;
  values[1].value.floatingValue = 1.5;
  values[2] = values[0];
  values[2].value.integerValue = 456;
  values[3] = values[1];
  values[3].value.floatingValue = -2.25;
  ASSERT_TRUE(publisher.PublishWithSequence(values, 100).ok());
  EXPECT_EQ(publisher.nextSampleCount(), 102u);

  std::array<std::uint8_t, 4096> storage{};
  IEC61850::RawEthernetFrameView ethernet;
  bool received = false;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    ASSERT_TRUE(capture.Receive(storage, &ethernet).ok());
    if (!ethernet.payload.empty()) {
      received = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(received) << "未收到SV发布报文";
  IEC61850::ProtocolSvStreamPlan plan;
  plan.streamId = 1;
  plan.svId = config.svId;
  plan.configRevision = config.configRevision;
  plan.sampleRate = config.sampleRate;
  plan.nofAsdu = config.nofAsdu;
  plan.members = config.members;
  std::array<IEC61850::ProtocolRealtimeValue, 4> decodedValues{};
  std::array<IEC61850::ProtocolSvFrameView, 2> frames{};
  std::size_t frameCount = 0;
  ASSERT_TRUE(IEC61850::DecodeSvPayload(
      ethernet.payload, plan, IEC61850Proto::NETWORK_CHANNEL_A,
      decodedValues, frames, &frameCount));
  ASSERT_EQ(frameCount, 2u);
  EXPECT_EQ(frames[0].sampleCount, 100u);
  EXPECT_EQ(frames[1].sampleCount, 101u);
  EXPECT_EQ(decodedValues[0].value.integerValue, -123);
  EXPECT_DOUBLE_EQ(decodedValues[3].value.floatingValue, -2.25);

  ASSERT_TRUE(publisher.Retransmit().ok());
  publisher.Close();
}

// 验证SV发布端在未打开、成员数量错误或配置非法时不发送并返回明确状态。
TEST(IEC61850RawProtocolStackTest, RejectsInvalidSvPublisherLifecycle) {
  auto config = MakeSvPublisherConfig();
  IEC61850::SvPublisher publisher(config);
  std::array<IEC61850::ProtocolRealtimeValue, 4> values{};
  EXPECT_EQ(publisher.Publish(values).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  config.members.front().encodedSize = 0;
  IEC61850::SvPublisher invalidMember(std::move(config));
  EXPECT_EQ(invalidMember.Open().error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  auto invalidAsdu = MakeSvPublisherConfig();
  invalidAsdu.nofAsdu = 0;
  IEC61850::SvPublisher invalidCount(std::move(invalidAsdu));
  EXPECT_EQ(invalidCount.Open().error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  auto invalidVlan = MakeSvPublisherConfig();
  invalidVlan.endpoint.vlanPriority = 1;
  IEC61850::SvPublisher invalidVlanPublisher(std::move(invalidVlan));
  EXPECT_EQ(invalidVlanPublisher.Open().error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证256点长BER SV单ASDU能够持续解码，并记录离线解码吞吐基线。
TEST(IEC61850RawProtocolStackTest, DecodesWideSvFrameRepeatedly) {
  constexpr std::size_t kMemberCount = 256;
  constexpr std::size_t kIterations = 1000;
  const auto payload = MakeWideSvPayload(kMemberCount);
  const auto plan = MakeWideSvPlan(kMemberCount);
  std::vector<IEC61850::ProtocolRealtimeValue> values(kMemberCount);
  std::vector<IEC61850::ProtocolSvFrameView> frames(1);

  const auto started = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    std::size_t frameCount = 0;
    ASSERT_TRUE(IEC61850::DecodeSvPayload(
        payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, frames,
        &frameCount));
    ASSERT_EQ(frameCount, 1u);
    ASSERT_EQ(frames[0].values.size(), kMemberCount);
    EXPECT_EQ(frames[0].values.front().value.integerValue, 0);
    EXPECT_EQ(frames[0].values.back().value.integerValue, 255);
  }
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  RecordProperty("sv_member_count", static_cast<int>(kMemberCount));
  RecordProperty("decode_iterations", static_cast<int>(kIterations));
  RecordProperty("elapsed_ms", elapsedMs.count());
  ASSERT_GT(elapsedMs.count(), 0);
}

// 验证可选真实压力模式下256点SV长帧能够持续经过网卡接收和协议解码。
TEST(IEC61850RawProtocolStackTest, LiveWideSvFramesRecordThroughput) {
  const char* configuredInterface = std::getenv("IEC61850_LIVE_INTERFACE");
  const char* performanceMode = std::getenv("IEC61850_LIVE_PERF");
  if (configuredInterface == nullptr || configuredInterface[0] == '\0' ||
      performanceMode == nullptr || std::string_view(performanceMode) != "1") {
    GTEST_SKIP() << "未启用IEC61850_LIVE_PERF=1或未设置真实网卡，跳过SV压力验收";
  }

  constexpr std::size_t kMemberCount = 256;
  constexpr std::size_t kFramesToSend = 1000;
  const std::string interfaceName(configuredInterface);
  auto stream = MakeWideSvPlan(kMemberCount);
  auto& endpoint = stream.endpoints.emplace_back();
  endpoint.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  endpoint.interfaceName = interfaceName;
  endpoint.destinationMac = "01:0c:cd:04:00:01";
  endpoint.appId = 0x4001;

  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("live-wide-sv");
  plan.config.set_enable_sv(true);
  plan.svStreams.push_back(stream);

  std::atomic<std::size_t> receivedFrames = 0;
  std::atomic<std::size_t> callbackFrames = 0;
  std::atomic<bool> invalidFrame = false;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onSvFrame = [&receivedFrames, &callbackFrames, &invalidFrame](
                            IEC61850::ProtocolSvFrameView frame) {
    callbackFrames.fetch_add(1, std::memory_order_relaxed);
    if (frame.values.size() != 256 || frame.asduCount != 1 ||
        frame.values.front().value.integerValue != 0 ||
        frame.values.back().value.integerValue != 255) {
      invalidFrame.store(true, std::memory_order_relaxed);
      return;
    }
    receivedFrames.fetch_add(1, std::memory_order_relaxed);
  };

  auto stack = IEC61850::MakeRawProtocolStack();
  ASSERT_TRUE(stack->StartIed(plan, callbacks).ok());
  IEC61850::RawEthernetFilter filter;
  filter.destinationMac = {0x01, 0x0c, 0xcd, 0x04, 0x00, 0x01};
  filter.etherType = 0x88ba;
  filter.appId = 0x4001;
  IEC61850::RawEthernetSocket sender;
  ASSERT_TRUE(sender.Open(interfaceName, filter).ok());
  const auto frame = MakeLiveEthernetFrame(
      filter, MakeWideSvPayload(kMemberCount), sender.localMac());

  const auto started = std::chrono::steady_clock::now();
  std::size_t sentFrames = 0;
  const auto sendDeadline = started + std::chrono::seconds(5);
  while (sentFrames < kFramesToSend &&
         std::chrono::steady_clock::now() < sendDeadline) {
    if (sender.Send(frame).ok()) {
      ++sentFrames;
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
  const auto receiveDeadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(5);
  while (receivedFrames.load(std::memory_order_relaxed) < sentFrames &&
         std::chrono::steady_clock::now() < receiveDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  RecordProperty("sv_member_count", static_cast<int>(kMemberCount));
  RecordProperty("sent_frames", static_cast<int>(sentFrames));
  RecordProperty("received_frames",
                 static_cast<int>(receivedFrames.load()));
  RecordProperty("callback_frames", static_cast<int>(callbackFrames.load()));
  RecordProperty("elapsed_ms", elapsedMs.count());
  EXPECT_FALSE(invalidFrame.load(std::memory_order_relaxed));
  EXPECT_GT(sentFrames, 0u);
  EXPECT_GT(receivedFrames.load(std::memory_order_relaxed), 0u);
  EXPECT_TRUE(stack->StopIed("live-wide-sv").ok());
}

// 验证SV序列数据长度异常时不会交付部分ASDU。
TEST(IEC61850RawProtocolStackTest, RejectsSvSequenceDataLengthMismatch) {
  const auto payload = MakeSvPayload(true);
  const auto plan = MakeSvPlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(4);
  std::vector<IEC61850::ProtocolSvFrameView> frames(2);
  std::size_t frameCount = 99;

  EXPECT_FALSE(IEC61850::DecodeSvPayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, frames,
      &frameCount));
  EXPECT_EQ(frameCount, 0u);
}

// 验证SV APPID载荷长度与实际接收长度不一致时不会交付任何ASDU。
TEST(IEC61850RawProtocolStackTest, RejectsSvPayloadLengthMismatch) {
  auto payload = MakeSvPayload();
  payload.push_back(0);
  const auto plan = MakeSvPlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(4);
  std::vector<IEC61850::ProtocolSvFrameView> frames(2);
  std::size_t frameCount = 99;

  EXPECT_FALSE(IEC61850::DecodeSvPayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, frames,
      &frameCount));
  EXPECT_EQ(frameCount, 0u);
}

// 验证SV外层缺少必需的noASDU字段时不会交付任何ASDU。
TEST(IEC61850RawProtocolStackTest, RejectsSvMissingNoAsdu) {
  const auto payload = MakeSvPayload(false, false, -1);
  const auto plan = MakeSvPlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(4);
  std::vector<IEC61850::ProtocolSvFrameView> frames(2);
  std::size_t frameCount = 99;

  EXPECT_FALSE(IEC61850::DecodeSvPayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, frames,
      &frameCount));
  EXPECT_EQ(frameCount, 0u);
}

// 验证SV外层声明的noASDU数量与实际计划不一致时不会交付任何ASDU。
TEST(IEC61850RawProtocolStackTest, RejectsSvNoAsduCountMismatch) {
  const auto payload = MakeSvPayload(false, false, 1);
  const auto plan = MakeSvPlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(4);
  std::vector<IEC61850::ProtocolSvFrameView> frames(2);
  std::size_t frameCount = 99;

  EXPECT_FALSE(IEC61850::DecodeSvPayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, frames,
      &frameCount));
  EXPECT_EQ(frameCount, 0u);
}

// 验证SV ASDU必需字段重复时不会交付任何ASDU。
TEST(IEC61850RawProtocolStackTest, RejectsDuplicateSvMandatoryField) {
  const auto payload = MakeSvPayload(false, true);
  const auto plan = MakeSvPlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(4);
  std::vector<IEC61850::ProtocolSvFrameView> frames(2);
  std::size_t frameCount = 99;

  EXPECT_FALSE(IEC61850::DecodeSvPayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, frames,
      &frameCount));
  EXPECT_EQ(frameCount, 0u);
}

// 验证SV的refrTm[5]必须是8字节UtcTime，不能被误当作smpRate接受。
TEST(IEC61850RawProtocolStackTest, RejectsInvalidSvReferenceTimeLength) {
  const auto payload = MakeSvPayload(false, false, 2, false, false, false,
                                     true);
  const auto plan = MakeSvPlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(4);
  std::vector<IEC61850::ProtocolSvFrameView> frames(2);
  std::size_t frameCount = 99;

  EXPECT_FALSE(IEC61850::DecodeSvPayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, frames,
      &frameCount));
  EXPECT_EQ(frameCount, 0u);
}

// 验证SV可选字段必须按refrTm[5]后接smpRate[6]的顺序出现。
TEST(IEC61850RawProtocolStackTest, RejectsReversedSvOptionalFields) {
  const auto payload = MakeSvPayload(false, false, 2, false, false, false,
                                     false, true);
  const auto plan = MakeSvPlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(4);
  std::vector<IEC61850::ProtocolSvFrameView> frames(2);
  std::size_t frameCount = 99;

  EXPECT_FALSE(IEC61850::DecodeSvPayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, frames,
      &frameCount));
  EXPECT_EQ(frameCount, 0u);
}

// 验证SV ASDU中出现未声明字段时不会忽略并交付整帧。
TEST(IEC61850RawProtocolStackTest, RejectsUnknownSvAsduField) {
  const auto payload = MakeSvPayload(false, false, 2, true);
  const auto plan = MakeSvPlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(4);
  std::vector<IEC61850::ProtocolSvFrameView> frames(2);
  std::size_t frameCount = 99;

  EXPECT_FALSE(IEC61850::DecodeSvPayload(
      payload, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, frames,
      &frameCount));
  EXPECT_EQ(frameCount, 0u);
}

// 验证SV SavPdu中出现未声明字段或尾随TLV时不会交付整帧。
TEST(IEC61850RawProtocolStackTest, RejectsUnknownOrTrailingSvOuterField) {
  const auto plan = MakeSvPlan();
  std::vector<IEC61850::ProtocolRealtimeValue> values(4);
  std::vector<IEC61850::ProtocolSvFrameView> frames(2);
  std::size_t frameCount = 99;

  const auto unknown = MakeSvPayload(false, false, 2, false, true);
  EXPECT_FALSE(IEC61850::DecodeSvPayload(
      unknown, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, frames,
      &frameCount));
  EXPECT_EQ(frameCount, 0u);

  const auto trailing = MakeSvPayload(false, false, 2, false, false, true);
  EXPECT_FALSE(IEC61850::DecodeSvPayload(
      trailing, plan, IEC61850Proto::NETWORK_CHANNEL_A, values, frames,
      &frameCount));
  EXPECT_EQ(frameCount, 0u);
}

// 验证GOOSE订阅缺少二层端点时在打开网卡前拒绝启动，不能进入假运行态。
TEST(IEC61850RawProtocolStackTest, RejectsGooseSubscriptionWithoutEndpoint) {
  auto stack = IEC61850::MakeRawProtocolStack();
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("invalid-goose-endpoint");
  plan.config.set_enable_goose(true);
  plan.gooseSubscriptions.emplace_back(
      MakeGooseValidationSubscription(1));
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onGooseFrame = [](IEC61850::ProtocolGooseFrameView) {};

  const auto status = stack->StartIed(std::move(plan), std::move(callbacks));
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(stack->StopIed("invalid-goose-endpoint").ok());
}

// 验证GOOSE订阅ID重复时在创建线程前拒绝启动，避免发布路由产生歧义。
TEST(IEC61850RawProtocolStackTest, RejectsDuplicateGooseSubscriptionId) {
  auto stack = IEC61850::MakeRawProtocolStack();
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("duplicate-goose-id");
  plan.config.set_enable_goose(true);
  plan.gooseSubscriptions.emplace_back(
      MakeGooseValidationSubscription(7));
  plan.gooseSubscriptions.emplace_back(
      MakeGooseValidationSubscription(7));
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onGooseFrame = [](IEC61850::ProtocolGooseFrameView) {};

  const auto status = stack->StartIed(std::move(plan), std::move(callbacks));
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(stack->StopIed("duplicate-goose-id").ok());
}

// 验证只发布GOOSE时不依赖接收回调，但发布端点缺失仍在创建网卡前拒绝。
TEST(IEC61850RawProtocolStackTest, RejectsGoosePublisherWithoutEndpoint) {
  auto stack = IEC61850::MakeRawProtocolStack();
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("invalid-goose-publisher-endpoint");
  plan.config.set_enable_goose(true);
  plan.goosePublishers.emplace_back(MakePublisherValidationPlan(8));

  const auto status =
      stack->StartIed(std::move(plan), IEC61850::ProtocolEventCallbacks{});
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(stack->StopIed("invalid-goose-publisher-endpoint").ok());
}

// 验证独立GOOSE发布端ID重复时不能进入协议栈运行态。
TEST(IEC61850RawProtocolStackTest, RejectsDuplicateGoosePublisherId) {
  auto stack = IEC61850::MakeRawProtocolStack();
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("duplicate-goose-publisher-id");
  plan.config.set_enable_goose(true);
  plan.goosePublishers.emplace_back(MakePublisherValidationPlan(9));
  plan.goosePublishers.emplace_back(MakePublisherValidationPlan(9));

  const auto status =
      stack->StartIed(std::move(plan), IEC61850::ProtocolEventCallbacks{});
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(stack->StopIed("duplicate-goose-publisher-id").ok());
}

// 验证仅有GOOSE订阅计划时不会推断本地发布器，发布请求必须明确拒绝。
TEST(IEC61850RawProtocolStackTest, DoesNotPublishFromSubscriptionOnlyPlan) {
  auto stack = IEC61850::MakeRawProtocolStack();
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("goose-subscription-only");
  plan.config.set_enable_goose(true);
  plan.gooseSubscriptions.emplace_back(MakeGooseLoopbackSubscription(17));
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onGooseFrame = [](IEC61850::ProtocolGooseFrameView) {};

  const auto started = stack->StartIed(plan, callbacks);
  if (!started.ok()) {
    if (started.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
        started.error_code() == grpc::StatusCode::UNAVAILABLE) {
      GTEST_SKIP() << "当前环境不允许打开lo上的AF_PACKET接收套接字: "
                   << started.error_message();
    }
    FAIL() << "仅订阅GOOSE计划启动失败: " << started.error_message();
  }

  IEC61850::ProtocolRealtimeValue value;
  value.valueType = IEC61850::ProtocolRealtimeValueType::BOOLEAN;
  value.value.booleanValue = true;
  const std::array values{value};

  IEC61850::ProtocolGoosePublishCommand legacyCommand;
  legacyCommand.subscriptionId = 17;
  legacyCommand.values = values;
  EXPECT_EQ(stack->PublishGoose("goose-subscription-only", legacyCommand)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  IEC61850::ProtocolGoosePublishCommand publisherCommand;
  publisherCommand.publisherId = 17;
  publisherCommand.subscriptionId = 17;
  publisherCommand.values = values;
  EXPECT_EQ(stack->PublishGoose("goose-subscription-only", publisherCommand)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(stack->StopIed("goose-subscription-only").ok());
}

// 验证只有本地GOOSE发布计划时能够发送报文，不依赖虚构的订阅路由。
TEST(IEC61850RawProtocolStackTest, PublishesFromPublisherOnlyPlan) {
  auto stack = IEC61850::MakeRawProtocolStack();
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("goose-publisher-only");
  plan.config.set_enable_goose(true);
  auto publisher = MakePublisherValidationPlan(19);
  auto& endpoint = publisher.endpoints.emplace_back();
  endpoint.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  endpoint.interfaceName = "lo";
  endpoint.destinationMac = "01:0c:cd:01:00:01";
  endpoint.appId = 0x1001;
  plan.goosePublishers.emplace_back(std::move(publisher));

  const auto started =
      stack->StartIed(plan, IEC61850::ProtocolEventCallbacks{});
  if (!started.ok()) {
    if (started.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
        started.error_code() == grpc::StatusCode::UNAVAILABLE) {
      GTEST_SKIP() << "当前环境不允许打开lo上的GOOSE发布套接字: "
                   << started.error_message();
    }
    FAIL() << "仅发布GOOSE计划启动失败: " << started.error_message();
  }

  IEC61850::RawEthernetFilter filter;
  filter.destinationMac = {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01};
  filter.etherType = 0x88b8;
  filter.appId = 0x1001;
  IEC61850::RawEthernetSocket capture;
  const auto captureStatus = capture.Open("lo", filter);
  if (!captureStatus.ok()) {
    EXPECT_TRUE(stack->StopIed("goose-publisher-only").ok());
    if (captureStatus.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
        captureStatus.error_code() == grpc::StatusCode::UNAVAILABLE) {
      GTEST_SKIP() << "当前环境不允许打开lo上的GOOSE抓包套接字: "
                   << captureStatus.error_message();
    }
    FAIL() << "GOOSE抓包套接字打开失败: " << captureStatus.error_message();
  }

  IEC61850::ProtocolRealtimeValue value;
  value.valueType = IEC61850::ProtocolRealtimeValueType::BOOLEAN;
  value.value.booleanValue = true;
  const std::array values{value};
  IEC61850::ProtocolGoosePublishCommand command;
  command.publisherId = 19;
  command.stateChanged = true;
  command.values = values;
  const auto published =
      stack->PublishGoose("goose-publisher-only", command);
  if (!published.ok() &&
      (published.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
       published.error_code() == grpc::StatusCode::UNAVAILABLE)) {
    EXPECT_TRUE(stack->StopIed("goose-publisher-only").ok());
    GTEST_SKIP() << "当前环境不允许在lo上发送GOOSE报文: "
                 << published.error_message();
  }
  ASSERT_TRUE(published.ok()) << published.error_message();

  auto decodePlan = MakeGooseLoopbackSubscription(19);
  std::vector<IEC61850::ProtocolRealtimeValue> decodedValues(1);
  IEC61850::ProtocolGooseFrameView decodedFrame;
  std::array<std::uint8_t, IEC61850::kRawEthernetReceiveBufferBytes> storage{};
  bool decoded = false;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    IEC61850::RawEthernetFrameView frame;
    const auto status = capture.Receive(storage, &frame);
    ASSERT_TRUE(status.ok()) << status.error_message();
    if (!frame.payload.empty() &&
        IEC61850::DecodeGoosePayload(
            frame.payload, decodePlan, IEC61850Proto::NETWORK_CHANNEL_A,
            decodedValues, &decodedFrame)) {
      decoded = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(decoded);
  if (decoded) {
    EXPECT_EQ(decodedFrame.subscriptionId, 19u);
    EXPECT_EQ(decodedFrame.appId, 0x1001u);
    EXPECT_EQ(decodedFrame.stateNumber, 1u);
    ASSERT_EQ(decodedFrame.values.size(), 1u);
    EXPECT_TRUE(decodedFrame.values.front().value.booleanValue);
  }
  EXPECT_TRUE(stack->StopIed("goose-publisher-only").ok());
}

// 验证GOOSE发布计划和订阅计划同时存在时，发送使用本地发布端点、接收只走订阅端点。
TEST(IEC61850RawProtocolStackTest, SeparatesGoosePublisherAndSubscriptionRoutes) {
  auto publisher = MakeGooseLoopbackPublisher(
      42, "01:0c:cd:01:02:02", static_cast<std::uint16_t>(0x2202));
  publisher.controlRef = "IED1LD0/LLN0$GO$local";
  publisher.dataSetRef = "IED1LD0/LLN0$local-events";
  publisher.goId = "LocalTrip";
  publisher.configRevision = 9;
  const auto decodePublisherPlan = MakeGooseDecodePlan(publisher);

  auto subscription = MakeGooseLoopbackSubscription(17);
  subscription.configRevision = 4;
  subscription.endpoints.front().destinationMac = "01:0c:cd:01:02:01";
  subscription.endpoints.front().appId = 0x2201;

  IEC61850::RawEthernetFilter publisherFilter;
  publisherFilter.destinationMac = {0x01, 0x0c, 0xcd, 0x01, 0x02, 0x02};
  publisherFilter.etherType = 0x88b8;
  publisherFilter.appId = 0x2202;
  IEC61850::RawEthernetSocket publisherCapture;
  auto status = publisherCapture.Open("lo", publisherFilter);
  if (!status.ok()) {
    if (status.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
        status.error_code() == grpc::StatusCode::UNAVAILABLE) {
      GTEST_SKIP() << "当前环境不允许打开lo上的GOOSE发布观测套接字: "
                   << status.error_message();
    }
    FAIL() << "GOOSE发布观测套接字打开失败: " << status.error_message();
  }

  IEC61850::RawEthernetFilter subscriptionFilter;
  subscriptionFilter.destinationMac = {0x01, 0x0c, 0xcd, 0x01, 0x02, 0x01};
  subscriptionFilter.etherType = 0x88b8;
  subscriptionFilter.appId = 0x2201;
  IEC61850::RawEthernetSocket subscriptionObserver;
  status = subscriptionObserver.Open("lo", subscriptionFilter);
  if (!status.ok()) {
    if (status.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
        status.error_code() == grpc::StatusCode::UNAVAILABLE) {
      GTEST_SKIP() << "当前环境不允许打开lo上的GOOSE订阅观测套接字: "
                   << status.error_message();
    }
    FAIL() << "GOOSE订阅观测套接字打开失败: " << status.error_message();
  }
  IEC61850::RawEthernetSocket subscriptionSender;
  status = subscriptionSender.Open("lo", subscriptionFilter);
  if (!status.ok()) {
    if (status.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
        status.error_code() == grpc::StatusCode::UNAVAILABLE) {
      GTEST_SKIP() << "当前环境不允许打开lo上的GOOSE订阅注入套接字: "
                   << status.error_message();
    }
    FAIL() << "GOOSE订阅注入套接字打开失败: " << status.error_message();
  }

  std::atomic<int> subscriptionCallbacks = 0;
  std::atomic<bool> validSubscriptionFrame = false;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onGooseFrame = [&subscription, &subscriptionCallbacks,
                            &validSubscriptionFrame](
                               IEC61850::ProtocolGooseFrameView frame) {
    if (frame.subscriptionId == subscription.subscriptionId &&
        frame.appId == subscription.endpoints.front().appId &&
        frame.gocbRef == subscription.controlRef &&
        frame.dataSetRef == subscription.dataSetRef &&
        frame.goId == subscription.goId && frame.configRevision == 4 &&
        frame.values.size() == 1 &&
        frame.values[0].valueType ==
            IEC61850::ProtocolRealtimeValueType::BOOLEAN &&
        frame.values[0].value.booleanValue) {
      validSubscriptionFrame.store(true, std::memory_order_relaxed);
    }
    subscriptionCallbacks.fetch_add(1, std::memory_order_relaxed);
  };

  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("goose-publisher-and-subscription");
  plan.config.set_enable_goose(true);
  plan.goosePublishers.push_back(publisher);
  plan.gooseSubscriptions.push_back(subscription);
  auto stack = IEC61850::MakeRawProtocolStack();
  const auto started = stack->StartIed(plan, callbacks);
  if (!started.ok()) {
    if (started.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
        started.error_code() == grpc::StatusCode::UNAVAILABLE) {
      GTEST_SKIP() << "当前环境不允许启动GOOSE发布/订阅双路套接字: "
                   << started.error_message();
    }
    FAIL() << "GOOSE发布/订阅双路计划启动失败: " << started.error_message();
  }

  IEC61850::ProtocolRealtimeValue value;
  value.valueType = IEC61850::ProtocolRealtimeValueType::BOOLEAN;
  value.value.booleanValue = true;
  const std::array values{value};
  IEC61850::ProtocolGoosePublishCommand publishCommand;
  publishCommand.publisherId = publisher.publisherId;
  publishCommand.values = values;
  auto published = stack->PublishGoose(
      "goose-publisher-and-subscription", publishCommand);
  if (!published.ok() &&
      (published.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
       published.error_code() == grpc::StatusCode::UNAVAILABLE)) {
    GTEST_SKIP() << "当前环境不允许在lo上发送AF_PACKET报文: "
                 << published.error_message();
  }
  ASSERT_TRUE(published.ok()) << published.error_message();

  std::array<std::uint8_t, 4096> publisherStorage{};
  IEC61850::RawEthernetFrameView publisherFrame;
  bool publisherReceived = false;
  ASSERT_TRUE(PollRawEthernetFrame(
                  &publisherCapture, publisherStorage, &publisherFrame,
                  std::chrono::milliseconds(1000), &publisherReceived)
                  .ok());
  ASSERT_TRUE(publisherReceived) << "未收到本地发布端点的GOOSE报文";
  std::array<IEC61850::ProtocolRealtimeValue, 1> decodedValues{};
  IEC61850::ProtocolGooseFrameView decodedFrame;
  ASSERT_TRUE(IEC61850::DecodeGoosePayload(
      publisherFrame.payload, decodePublisherPlan,
      IEC61850Proto::NETWORK_CHANNEL_A, decodedValues, &decodedFrame));
  EXPECT_EQ(decodedFrame.appId, 0x2202u);
  EXPECT_EQ(decodedFrame.gocbRef, publisher.controlRef);
  EXPECT_EQ(decodedFrame.dataSetRef, publisher.dataSetRef);
  EXPECT_EQ(decodedFrame.goId, publisher.goId);
  EXPECT_EQ(decodedFrame.configRevision, publisher.configRevision);

  std::array<std::uint8_t, 4096> unexpectedStorage{};
  IEC61850::RawEthernetFrameView unexpectedFrame;
  bool unexpectedSubscriptionFrame = false;
  ASSERT_TRUE(PollRawEthernetFrame(
                  &subscriptionObserver, unexpectedStorage, &unexpectedFrame,
                  std::chrono::milliseconds(20), &unexpectedSubscriptionFrame)
                  .ok());
  EXPECT_FALSE(unexpectedSubscriptionFrame)
      << "本地发布报文错误地发往GOOSE订阅端点";
  EXPECT_EQ(subscriptionCallbacks.load(std::memory_order_relaxed), 0);

  auto incomingPayload = MakeGoosePayload();
  incomingPayload[0] = static_cast<std::uint8_t>(subscriptionFilter.appId >> 8);
  incomingPayload[1] = static_cast<std::uint8_t>(subscriptionFilter.appId);
  const auto incomingFrame = MakeLiveEthernetFrame(
      subscriptionFilter, incomingPayload, subscriptionSender.localMac());
  const auto callbackDeadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(1000);
  while (std::chrono::steady_clock::now() < callbackDeadline &&
         subscriptionCallbacks.load(std::memory_order_relaxed) == 0) {
    const auto sendStatus = subscriptionSender.Send(incomingFrame);
    if (!sendStatus.ok() &&
        (sendStatus.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
         sendStatus.error_code() == grpc::StatusCode::UNAVAILABLE)) {
      GTEST_SKIP() << "当前环境不允许在lo上注入GOOSE订阅报文: "
                   << sendStatus.error_message();
    }
    ASSERT_TRUE(sendStatus.ok()) << sendStatus.error_message();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_GE(subscriptionCallbacks.load(std::memory_order_relaxed), 1);
  EXPECT_TRUE(validSubscriptionFrame.load(std::memory_order_relaxed));
  EXPECT_TRUE(stack->StopIed("goose-publisher-and-subscription").ok());
}

// 验证GOOSE Quality计划必须声明INT64，不能把BIT STRING当作BOOL或浮点成员。
TEST(IEC61850RawProtocolStackTest, RejectsGooseQualityWithWrongValueType) {
  auto stack = IEC61850::MakeRawProtocolStack();
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("invalid-goose-quality-type");
  plan.config.set_enable_goose(true);
  auto subscription = MakeGooseLoopbackSubscription(18);
  subscription.members[0].qualityValue = true;
  plan.gooseSubscriptions.emplace_back(std::move(subscription));
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onGooseFrame = [](IEC61850::ProtocolGooseFrameView) {};

  const auto status = stack->StartIed(std::move(plan), std::move(callbacks));
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(stack->StopIed("invalid-goose-quality-type").ok());
}

// 验证SV采样流缺少二层端点时在打开网卡前拒绝启动，不能产生空采样线程。
TEST(IEC61850RawProtocolStackTest, RejectsSvStreamWithoutEndpoint) {
  auto stack = IEC61850::MakeRawProtocolStack();
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("invalid-sv-endpoint");
  plan.config.set_enable_sv(true);
  plan.svStreams.emplace_back(MakeSvValidationStream(1));
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onSvFrame = [](IEC61850::ProtocolSvFrameView) {};

  const auto status = stack->StartIed(std::move(plan), std::move(callbacks));
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(stack->StopIed("invalid-sv-endpoint").ok());
}

// 验证SV采样流ID重复时在创建线程前拒绝启动，避免状态统计路由串流。
TEST(IEC61850RawProtocolStackTest, RejectsDuplicateSvStreamId) {
  auto stack = IEC61850::MakeRawProtocolStack();
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("duplicate-sv-id");
  plan.config.set_enable_sv(true);
  plan.svStreams.emplace_back(MakeSvValidationStream(4));
  plan.svStreams.emplace_back(MakeSvValidationStream(4));
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onSvFrame = [](IEC61850::ProtocolSvFrameView) {};

  const auto status = stack->StartIed(std::move(plan), std::move(callbacks));
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(stack->StopIed("duplicate-sv-id").ok());
}

// 验证MMS已经启动后GOOSE启动失败时会回滚全部资源，同名IED仍可再次启动。
TEST(IEC61850RawProtocolStackTest, RollsBackPartialStartBeforeRetry) {
  auto stack = IEC61850::MakeRawProtocolStack();
  auto plan = MakePartialStartPlan();
  std::atomic<int> connectionEvents = 0;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection =
      [&connectionEvents](IEC61850::MmsConnectionEvent) {
        ++connectionEvents;
      };
  callbacks.onGooseFrame = [](IEC61850::ProtocolGooseFrameView) {};

  const auto failed = stack->StartIed(plan, callbacks);
  EXPECT_FALSE(failed.ok());
  const auto eventsAfterFailure = connectionEvents.load();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(connectionEvents.load(), eventsAfterFailure);

  plan.config.set_enable_mms(false);
  plan.config.set_enable_goose(false);
  plan.networkBindings.clear();
  plan.gooseSubscriptions.clear();
  EXPECT_TRUE(stack->StartIed(std::move(plan), std::move(callbacks)).ok());
  EXPECT_TRUE(stack->StopIed("partial-start").ok());
}

// 验证GOOSE自动重发间隔按指数曲线递增并在上限处饱和。
TEST(IEC61850RawProtocolStackTest, AdvancesGooseRetransmitCurve) {
  EXPECT_EQ(IEC61850::NextGooseRetransmitIntervalMs(0), 1u);
  EXPECT_EQ(IEC61850::NextGooseRetransmitIntervalMs(1), 2u);
  EXPECT_EQ(IEC61850::NextGooseRetransmitIntervalMs(2), 4u);
  EXPECT_EQ(IEC61850::NextGooseRetransmitIntervalMs(512), 1000u);
  EXPECT_EQ(IEC61850::NextGooseRetransmitIntervalMs(1000), 1000u);
  EXPECT_EQ(IEC61850::NextGooseRetransmitIntervalMs(1, 0), 0u);
}

// 验证真实网卡注入的GOOSE和SV帧能够经过原始协议栈线程进入回调。
TEST(IEC61850RawProtocolStackTest, LiveGooseAndSvFramesReachCallbacks) {
  const char* configuredInterface = std::getenv("IEC61850_LIVE_INTERFACE");
  if (configuredInterface == nullptr || configuredInterface[0] == '\0') {
    GTEST_SKIP() << "未设置IEC61850_LIVE_INTERFACE，跳过真实GOOSE/SV验收";
  }

  const std::string interfaceName(configuredInterface);
  auto goosePlan = MakePlan();
  ConfigureLiveGoosePlan(&goosePlan, interfaceName);
  auto svPlan = MakeSvPlan();
  ConfigureLiveSvPlan(&svPlan, interfaceName);

  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("live-goose-sv");
  plan.config.set_enable_mms(false);
  plan.config.set_enable_goose(true);
  plan.config.set_enable_sv(true);
  plan.gooseSubscriptions.push_back(goosePlan);
  plan.svStreams.push_back(svPlan);

  std::atomic<int> gooseCallbacks = 0;
  std::atomic<int> svCallbacks = 0;
  std::atomic<std::uint32_t> gooseStateNumber = 0;
  std::atomic<std::uint32_t> svAsduCount = 0;
  std::atomic<bool> kernelTimestampSeen = false;
  std::atomic<bool> gooseValueValid = false;
  std::atomic<bool> svFirstValueValid = false;
  std::atomic<bool> svSecondValueValid = false;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onGooseFrame = [&gooseCallbacks, &gooseStateNumber,
                            &gooseValueValid, &kernelTimestampSeen](
                               IEC61850::ProtocolGooseFrameView frame) {
    if (frame.kernelTimestampNs > 0) {
      kernelTimestampSeen.store(true, std::memory_order_relaxed);
    }
    gooseStateNumber.store(frame.stateNumber, std::memory_order_relaxed);
    if (frame.values.size() == 1 &&
        frame.values[0].valueType ==
            IEC61850::ProtocolRealtimeValueType::BOOLEAN &&
        frame.values[0].value.booleanValue && frame.appId == 0x1001 &&
        frame.gocbRef == "IED1LD0/LLN0$GO$gcb1" &&
        frame.dataSetRef == "IED1LD0/LLN0$events" && frame.goId == "Trip") {
      gooseValueValid.store(true, std::memory_order_relaxed);
    }
    gooseCallbacks.fetch_add(1, std::memory_order_relaxed);
  };
  callbacks.onSvFrame = [&svCallbacks, &svAsduCount, &svFirstValueValid,
                         &svSecondValueValid, &kernelTimestampSeen](
                            IEC61850::ProtocolSvFrameView frame) {
    if (frame.kernelTimestampNs > 0) {
      kernelTimestampSeen.store(true, std::memory_order_relaxed);
    }
    svAsduCount.store(frame.asduCount, std::memory_order_relaxed);
    if (frame.values.size() == 2 && frame.appId == 0x4001 &&
        frame.svId == "MU01" &&
        frame.values[0].valueType ==
            IEC61850::ProtocolRealtimeValueType::INTEGER &&
        frame.values[1].valueType ==
            IEC61850::ProtocolRealtimeValueType::FLOATING) {
      if (frame.asduIndex == 0 && frame.sampleCount == 10 &&
          frame.values[0].value.integerValue == 123 &&
          frame.values[1].value.floatingValue == 1.5) {
        svFirstValueValid.store(true, std::memory_order_relaxed);
      }
      if (frame.asduIndex == 1 && frame.sampleCount == 10 &&
          frame.values[0].value.integerValue == 124 &&
          frame.values[1].value.floatingValue == 1.5) {
        svSecondValueValid.store(true, std::memory_order_relaxed);
      }
    }
    svCallbacks.fetch_add(1, std::memory_order_relaxed);
  };

  auto stack = IEC61850::MakeRawProtocolStack();
  ASSERT_TRUE(stack->StartIed(plan, callbacks).ok());

  IEC61850::RawEthernetFilter gooseFilter;
  gooseFilter.destinationMac =
      {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01};
  gooseFilter.etherType = 0x88b8;
  gooseFilter.appId = 0x1001;
  IEC61850::RawEthernetSocket gooseSender;
  ASSERT_TRUE(gooseSender.Open(interfaceName, gooseFilter).ok());

  IEC61850::RawEthernetFilter svFilter;
  svFilter.destinationMac = {0x01, 0x0c, 0xcd, 0x04, 0x00, 0x01};
  svFilter.etherType = 0x88ba;
  svFilter.appId = 0x4001;
  IEC61850::RawEthernetSocket svSender;
  ASSERT_TRUE(svSender.Open(interfaceName, svFilter).ok());

  const auto gooseFrame = MakeLiveEthernetFrame(
      gooseFilter, MakeGoosePayload(), gooseSender.localMac());
  const auto svFrame = MakeLiveEthernetFrame(
      svFilter, MakeSvPayload(), svSender.localMac());
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(1000);
  while (std::chrono::steady_clock::now() < deadline &&
         (gooseCallbacks.load(std::memory_order_relaxed) == 0 ||
          svCallbacks.load(std::memory_order_relaxed) < 2)) {
    ASSERT_TRUE(gooseSender.Send(gooseFrame).ok());
    ASSERT_TRUE(svSender.Send(svFrame).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_GE(gooseCallbacks.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(gooseStateNumber.load(std::memory_order_relaxed), 1u);
  EXPECT_TRUE(gooseValueValid.load(std::memory_order_relaxed));
  EXPECT_GE(svCallbacks.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(svAsduCount.load(std::memory_order_relaxed), 2u);
  EXPECT_TRUE(svFirstValueValid.load(std::memory_order_relaxed));
  EXPECT_TRUE(svSecondValueValid.load(std::memory_order_relaxed));
  EXPECT_TRUE(kernelTimestampSeen.load(std::memory_order_relaxed));
  EXPECT_TRUE(stack->StopIed("live-goose-sv").ok());
}

// 验证真实网卡上的802.1Q VLAN GOOSE和SV帧能够通过过滤并进入回调。
TEST(IEC61850RawProtocolStackTest, LiveTaggedGooseAndSvFramesReachCallbacks) {
  const char* configuredInterface = std::getenv("IEC61850_LIVE_INTERFACE");
  if (configuredInterface == nullptr || configuredInterface[0] == '\0') {
    GTEST_SKIP() << "未设置IEC61850_LIVE_INTERFACE，跳过VLAN GOOSE/SV验收";
  }

  const std::string interfaceName(configuredInterface);
  auto goosePlan = MakePlan();
  ConfigureLiveGoosePlan(&goosePlan, interfaceName, true);
  auto svPlan = MakeSvPlan();
  ConfigureLiveSvPlan(&svPlan, interfaceName, true);

  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("live-tagged-goose-sv");
  plan.config.set_enable_goose(true);
  plan.config.set_enable_sv(true);
  plan.gooseSubscriptions.push_back(goosePlan);
  plan.svStreams.push_back(svPlan);

  std::atomic<int> gooseCallbacks = 0;
  std::atomic<int> svCallbacks = 0;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onGooseFrame = [&gooseCallbacks](
                               IEC61850::ProtocolGooseFrameView frame) {
    if (frame.appId == 0x1001 && frame.values.size() == 1) {
      gooseCallbacks.fetch_add(1, std::memory_order_relaxed);
    }
  };
  callbacks.onSvFrame = [&svCallbacks](IEC61850::ProtocolSvFrameView frame) {
    if (frame.appId == 0x4001 && frame.asduCount == 2) {
      svCallbacks.fetch_add(1, std::memory_order_relaxed);
    }
  };

  auto stack = IEC61850::MakeRawProtocolStack();
  ASSERT_TRUE(stack->StartIed(plan, callbacks).ok());

  IEC61850::RawEthernetFilter gooseFilter;
  gooseFilter.destinationMac = {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01};
  gooseFilter.etherType = 0x88b8;
  gooseFilter.appId = 0x1001;
  gooseFilter.vlanTagged = true;
  gooseFilter.vlanId = 7;
  gooseFilter.vlanPriority = 4;
  IEC61850::RawEthernetSocket gooseSender;
  ASSERT_TRUE(gooseSender.Open(interfaceName, gooseFilter).ok());

  IEC61850::RawEthernetFilter svFilter;
  svFilter.destinationMac = {0x01, 0x0c, 0xcd, 0x04, 0x00, 0x01};
  svFilter.etherType = 0x88ba;
  svFilter.appId = 0x4001;
  svFilter.vlanTagged = true;
  svFilter.vlanId = 7;
  svFilter.vlanPriority = 4;
  IEC61850::RawEthernetSocket svSender;
  ASSERT_TRUE(svSender.Open(interfaceName, svFilter).ok());

  const auto gooseFrame = MakeLiveEthernetFrame(
      gooseFilter, MakeGoosePayload(), gooseSender.localMac());
  const auto svFrame = MakeLiveEthernetFrame(
      svFilter, MakeSvPayload(), svSender.localMac());
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(1000);
  while (std::chrono::steady_clock::now() < deadline &&
         (gooseCallbacks.load(std::memory_order_relaxed) == 0 ||
          svCallbacks.load(std::memory_order_relaxed) < 2)) {
    ASSERT_TRUE(gooseSender.Send(gooseFrame).ok());
    ASSERT_TRUE(svSender.Send(svFrame).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_GE(gooseCallbacks.load(std::memory_order_relaxed), 1);
  EXPECT_GE(svCallbacks.load(std::memory_order_relaxed), 2);
  EXPECT_TRUE(stack->StopIed("live-tagged-goose-sv").ok());
}

// 验证GOOSE和SV在A/B两张真实网卡上均能接收，并正确携带通道标识。
TEST(IEC61850RawProtocolStackTest, LiveDualNetworkFramesReachBothChannels) {
  const char* interfaceA = std::getenv("IEC61850_LIVE_INTERFACE");
  const char* interfaceB = std::getenv("IEC61850_LIVE_INTERFACE_B");
  if (interfaceA == nullptr || interfaceA[0] == '\0' || interfaceB == nullptr ||
      interfaceB[0] == '\0') {
    GTEST_SKIP() << "未同时设置IEC61850_LIVE_INTERFACE和IEC61850_LIVE_INTERFACE_B，"
                    "跳过A/B网验收";
  }

  const std::string nameA(interfaceA);
  const std::string nameB(interfaceB);
  auto goosePlan = MakePlan();
  ConfigureLiveGoosePlan(&goosePlan, nameA);
  auto& gooseEndpointB = goosePlan.endpoints.emplace_back();
  gooseEndpointB.channel = IEC61850Proto::NETWORK_CHANNEL_B;
  gooseEndpointB.interfaceName = nameB;
  gooseEndpointB.destinationMac = "01:0c:cd:01:00:01";
  gooseEndpointB.appId = 0x1001;
  auto svPlan = MakeSvPlan();
  ConfigureLiveSvPlan(&svPlan, nameA);
  auto& svEndpointB = svPlan.endpoints.emplace_back();
  svEndpointB.channel = IEC61850Proto::NETWORK_CHANNEL_B;
  svEndpointB.interfaceName = nameB;
  svEndpointB.destinationMac = "01:0c:cd:04:00:01";
  svEndpointB.appId = 0x4001;

  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("live-dual-network");
  plan.config.set_enable_goose(true);
  plan.config.set_enable_sv(true);
  plan.gooseSubscriptions.push_back(goosePlan);
  plan.svStreams.push_back(svPlan);

  std::atomic<int> gooseA = 0;
  std::atomic<int> gooseB = 0;
  std::atomic<int> svA = 0;
  std::atomic<int> svB = 0;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onGooseFrame = [&gooseA, &gooseB](
                               IEC61850::ProtocolGooseFrameView frame) {
    auto* counter = frame.channel == IEC61850Proto::NETWORK_CHANNEL_A
                        ? &gooseA
                        : &gooseB;
    counter->fetch_add(1, std::memory_order_relaxed);
  };
  callbacks.onSvFrame = [&svA, &svB](IEC61850::ProtocolSvFrameView frame) {
    auto* counter = frame.channel == IEC61850Proto::NETWORK_CHANNEL_A ? &svA
                                                                       : &svB;
    counter->fetch_add(1, std::memory_order_relaxed);
  };

  auto stack = IEC61850::MakeRawProtocolStack();
  ASSERT_TRUE(stack->StartIed(plan, callbacks).ok());

  IEC61850::RawEthernetFilter gooseFilter;
  gooseFilter.destinationMac = {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01};
  gooseFilter.etherType = 0x88b8;
  gooseFilter.appId = 0x1001;
  IEC61850::RawEthernetSocket gooseSenderA;
  IEC61850::RawEthernetSocket gooseSenderB;
  ASSERT_TRUE(gooseSenderA.Open(nameA, gooseFilter).ok());
  ASSERT_TRUE(gooseSenderB.Open(nameB, gooseFilter).ok());

  IEC61850::RawEthernetFilter svFilter;
  svFilter.destinationMac = {0x01, 0x0c, 0xcd, 0x04, 0x00, 0x01};
  svFilter.etherType = 0x88ba;
  svFilter.appId = 0x4001;
  IEC61850::RawEthernetSocket svSenderA;
  IEC61850::RawEthernetSocket svSenderB;
  ASSERT_TRUE(svSenderA.Open(nameA, svFilter).ok());
  ASSERT_TRUE(svSenderB.Open(nameB, svFilter).ok());

  const auto gooseFrameA = MakeLiveEthernetFrame(
      gooseFilter, MakeGoosePayload(), gooseSenderA.localMac());
  const auto gooseFrameB = MakeLiveEthernetFrame(
      gooseFilter, MakeGoosePayload(), gooseSenderB.localMac());
  const auto svFrameA = MakeLiveEthernetFrame(
      svFilter, MakeSvPayload(), svSenderA.localMac());
  const auto svFrameB = MakeLiveEthernetFrame(
      svFilter, MakeSvPayload(), svSenderB.localMac());
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(1000);
  while (std::chrono::steady_clock::now() < deadline &&
         (gooseA.load(std::memory_order_relaxed) == 0 ||
          gooseB.load(std::memory_order_relaxed) == 0 ||
          svA.load(std::memory_order_relaxed) < 2 ||
          svB.load(std::memory_order_relaxed) < 2)) {
    ASSERT_TRUE(gooseSenderA.Send(gooseFrameA).ok());
    ASSERT_TRUE(gooseSenderB.Send(gooseFrameB).ok());
    ASSERT_TRUE(svSenderA.Send(svFrameA).ok());
    ASSERT_TRUE(svSenderB.Send(svFrameB).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_GE(gooseA.load(std::memory_order_relaxed), 1);
  EXPECT_GE(gooseB.load(std::memory_order_relaxed), 1);
  EXPECT_GE(svA.load(std::memory_order_relaxed), 2);
  EXPECT_GE(svB.load(std::memory_order_relaxed), 2);
  EXPECT_TRUE(stack->StopIed("live-dual-network").ok());
}
