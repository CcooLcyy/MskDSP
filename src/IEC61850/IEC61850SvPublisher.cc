#include "IEC61850SvPublisher.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

#include <net/ethernet.h>

#include "Logger.h"

namespace IEC61850 {
namespace {

bool ValidMember(const ProtocolSvMemberPlan& member) {
  if (member.dataRef.empty() ||
      member.fc == IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED) {
    return false;
  }
  switch (member.encoding) {
    case ProtocolSvMemberEncoding::BOOLEAN:
      return member.valueType == IEC61850Proto::POINT_VALUE_TYPE_BOOL &&
             member.encodedSize == 1;
    case ProtocolSvMemberEncoding::SIGNED_INTEGER:
    case ProtocolSvMemberEncoding::UNSIGNED_INTEGER:
      return member.valueType == IEC61850Proto::POINT_VALUE_TYPE_INT64 &&
             member.encodedSize >= 1 && member.encodedSize <= 8;
    case ProtocolSvMemberEncoding::FLOATING_POINT:
      return member.valueType == IEC61850Proto::POINT_VALUE_TYPE_DOUBLE &&
             (member.encodedSize == 4 || member.encodedSize == 8);
  }
  return false;
}

}  // namespace

SvPublisher::SvPublisher(SvPublisherConfig config)
    : config_(std::move(config)) {
  if (config_.nofAsdu != 0 && !config_.members.empty() &&
      config_.nofAsdu <= std::numeric_limits<std::size_t>::max() /
                              config_.members.size() &&
      config_.nofAsdu <= 1024) {
    currentValues_.resize(config_.members.size() * config_.nofAsdu);
    currentSampleCounts_.resize(config_.nofAsdu);
    pendingSampleCounts_.resize(config_.nofAsdu);
  }
  const auto parsed = ParseRawMac(config_.endpoint.destinationMac);
  if (parsed.has_value()) {
    destinationMac_ = *parsed;
    destinationMacValid_ = true;
  }
}

SvPublisher::~SvPublisher() { Close(); }

grpc::Status SvPublisher::ValidateConfig() const {
  if (!destinationMacValid_ || config_.endpoint.interfaceName.empty() ||
      config_.endpoint.appId == 0 || config_.svId.empty() ||
      config_.configRevision == 0 || config_.sampleRate == 0 ||
      config_.nofAsdu == 0 || config_.members.empty() ||
      config_.nofAsdu > 1024 ||
      config_.nofAsdu > std::numeric_limits<std::size_t>::max() /
                             config_.members.size() ||
      currentValues_.size() != config_.members.size() * config_.nofAsdu ||
      (config_.endpoint.vlanTagged && config_.endpoint.vlanId > 4095) ||
      (!config_.endpoint.vlanTagged &&
       (config_.endpoint.vlanId != 0 || config_.endpoint.vlanPriority != 0)) ||
      config_.endpoint.vlanPriority > 7) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "SV发布参数不完整或二层地址无效");
  }
  for (const auto& member : config_.members) {
    if (!ValidMember(member)) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "SV发布DataSet成员编码参数无效");
    }
  }
  return grpc::Status::OK;
}

grpc::Status SvPublisher::Open() {
  Close();
  const auto configStatus = ValidateConfig();
  if (!configStatus.ok()) {
    return configStatus;
  }
  RawEthernetFilter filter;
  filter.destinationMac = destinationMac_;
  filter.etherType = 0x88ba;
  filter.appId = config_.endpoint.appId;
  filter.vlanTagged = config_.endpoint.vlanTagged;
  filter.vlanId = config_.endpoint.vlanId;
  if (config_.endpoint.vlanTagged) {
    filter.vlanPriority = config_.endpoint.vlanPriority;
  }
  const auto status = socket_.Open(config_.endpoint.interfaceName, filter,
                                  kRawEthernetReceiveBufferBytes);
  if (!status.ok()) {
    return status;
  }
  LOG_INFO("IEC61850 SV发布端已打开: 网卡={}, APPID={}, svID={}",
           config_.endpoint.interfaceName, config_.endpoint.appId,
           config_.svId);
  return grpc::Status::OK;
}

void SvPublisher::Close() noexcept {
  socket_.Close();
  payloadSize_ = 0;
  frameSize_ = 0;
  nextSampleCount_ = 0;
  referenceTimeMs_ = 0;
  hasValues_ = false;
  std::fill(currentSampleCounts_.begin(), currentSampleCounts_.end(), 0);
}

bool SvPublisher::IsOpen() const noexcept { return socket_.IsOpen(); }

grpc::Status SvPublisher::Publish(
    std::span<const ProtocolRealtimeValue> values) {
  return PublishWithSequence(values, nextSampleCount_);
}

grpc::Status SvPublisher::PublishWithSequence(
    std::span<const ProtocolRealtimeValue> values,
    std::uint16_t startingSampleCount) {
  if (!IsOpen()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "SV发布网卡尚未打开");
  }
  if (values.size() != currentValues_.size()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "SV发布DataSet成员数量或ASDU数量不匹配");
  }
  for (std::size_t index = 0; index < pendingSampleCounts_.size(); ++index) {
    pendingSampleCounts_[index] = static_cast<std::uint16_t>(
        startingSampleCount + static_cast<std::uint16_t>(index));
  }
  const auto timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
  const auto status = EncodeAndSend(values, pendingSampleCounts_, timestampMs);
  if (!status.ok()) {
    return status;
  }
  std::copy(values.begin(), values.end(), currentValues_.begin());
  std::copy(pendingSampleCounts_.begin(), pendingSampleCounts_.end(),
            currentSampleCounts_.begin());
  nextSampleCount_ = static_cast<std::uint16_t>(
      startingSampleCount + static_cast<std::uint16_t>(pendingSampleCounts_.size()));
  referenceTimeMs_ = timestampMs;
  hasValues_ = true;
  return grpc::Status::OK;
}

grpc::Status SvPublisher::Retransmit() {
  if (!IsOpen() || !hasValues_) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "SV尚未有可重发的有效采样帧");
  }
  return EncodeAndSend(currentValues_, currentSampleCounts_, referenceTimeMs_);
}

grpc::Status SvPublisher::EncodeAndSend(
    std::span<const ProtocolRealtimeValue> values,
    std::span<const std::uint16_t> sampleCounts,
    std::int64_t referenceTimeMs) {
  SvPublishRequest request;
  request.svId = config_.svId;
  request.configRevision = config_.configRevision;
  request.sampleRate = config_.sampleRate;
  request.sampleSynchronization = config_.sampleSynchronization;
  request.sampleCounts = sampleCounts;
  request.values = values;
  request.members = config_.members;
  request.referenceTimeMs = referenceTimeMs;
  if (!EncodeSvPayload(request, config_.endpoint.appId, payload_,
                       &payloadSize_)) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "SV发布PDU超过预分配报文缓冲或编码失败");
  }
  auto status = BuildEthernetFrame(payloadSize_, &frameSize_);
  if (!status.ok()) {
    return status;
  }
  status = socket_.Send(std::span<const std::uint8_t>(frame_.data(), frameSize_));
  if (!status.ok()) {
    LOG_WARNING("IEC61850发送SV报文失败: APPID={}, smpCnt={}, 字节数={}, 原因={}",
                config_.endpoint.appId,
                sampleCounts.empty() ? 0 : sampleCounts.front(), frameSize_,
                status.error_message());
    return status;
  }
  LOG_DEBUG("IEC61850发送SV报文: APPID={}, smpCnt={}, ASDU数量={}, 字节数={}",
            config_.endpoint.appId,
            sampleCounts.empty() ? 0 : sampleCounts.front(),
            sampleCounts.size(), frameSize_);
  return grpc::Status::OK;
}

grpc::Status SvPublisher::BuildEthernetFrame(std::size_t payloadSize,
                                             std::size_t* frameSize) noexcept {
  if (frameSize == nullptr || !destinationMacValid_ ||
      payloadSize > frame_.size() - 18) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "SV二层发送报文超过预分配缓冲");
  }
  std::copy(destinationMac_.begin(), destinationMac_.end(), frame_.begin());
  const auto sourceMac = socket_.localMac();
  std::copy(sourceMac.begin(), sourceMac.end(), frame_.begin() + ETH_ALEN);
  std::size_t offset = 2 * ETH_ALEN;
  if (config_.endpoint.vlanTagged) {
    frame_[offset++] = 0x81;
    frame_[offset++] = 0x00;
    const auto tci = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(config_.endpoint.vlanPriority) << 13) |
        config_.endpoint.vlanId);
    frame_[offset++] = static_cast<std::uint8_t>(tci >> 8);
    frame_[offset++] = static_cast<std::uint8_t>(tci);
  }
  frame_[offset++] = 0x88;
  frame_[offset++] = 0xba;
  if (payloadSize > frame_.size() - offset) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "SV二层发送载荷超过预分配缓冲");
  }
  std::copy_n(payload_.data(), payloadSize, frame_.data() + offset);
  *frameSize = offset + payloadSize;
  return grpc::Status::OK;
}

std::uint16_t SvPublisher::nextSampleCount() const noexcept {
  return nextSampleCount_;
}

}  // namespace IEC61850
