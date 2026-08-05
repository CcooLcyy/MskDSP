#include "IEC61850GoosePublisher.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

#include <net/ethernet.h>

#include "Logger.h"

namespace IEC61850 {

std::uint32_t NextGooseRetransmitIntervalMs(
    std::uint32_t currentIntervalMs, std::uint32_t maxIntervalMs) noexcept {
  if (maxIntervalMs == 0) {
    return 0;
  }
  if (currentIntervalMs == 0) {
    return std::min(kGooseInitialRetransmitIntervalMs, maxIntervalMs);
  }
  if (currentIntervalMs >= maxIntervalMs ||
      currentIntervalMs > maxIntervalMs / 2) {
    return maxIntervalMs;
  }
  return currentIntervalMs * 2;
}

GoosePublisher::GoosePublisher(GoosePublisherConfig config)
    : config_(std::move(config)),
      destinationMac_(ParseRawMac(config_.endpoint.destinationMac)),
      currentValues_(config_.memberCount) {}

GoosePublisher::~GoosePublisher() { Close(); }

grpc::Status GoosePublisher::Open() {
  Close();
  if (!destinationMac_.has_value() || config_.endpoint.interfaceName.empty() ||
      config_.endpoint.appId == 0 || config_.gocbRef.empty() ||
      config_.dataSetRef.empty() || config_.goId.empty() ||
      config_.configRevision == 0 || config_.timeAllowedToLiveMs == 0 ||
      config_.memberCount == 0 ||
      config_.members.size() != config_.memberCount ||
      (config_.endpoint.vlanTagged && config_.endpoint.vlanId > 4095) ||
      config_.endpoint.vlanPriority > 7) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "GOOSE发布参数不完整或二层地址无效");
  }
  RawEthernetFilter filter;
  filter.destinationMac = *destinationMac_;
  filter.etherType = 0x88b8;
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
  LOG_INFO("IEC61850 GOOSE发布端已打开: 网卡={}, APPID={}",
           config_.endpoint.interfaceName, config_.endpoint.appId);
  return grpc::Status::OK;
}

void GoosePublisher::Close() noexcept {
  socket_.Close();
  payloadSize_ = 0;
  frameSize_ = 0;
  stateNumber_ = 0;
  sequenceNumber_ = 0;
  timestampMs_ = 0;
  hasValues_ = false;
}

bool GoosePublisher::IsOpen() const noexcept { return socket_.IsOpen(); }

grpc::Status GoosePublisher::Publish(
    std::span<const ProtocolRealtimeValue> values, bool stateChanged) {
  if (!IsOpen()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "GOOSE发布网卡尚未打开");
  }
  if (values.size() != currentValues_.size()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "GOOSE发布数据集成员数量不匹配");
  }
  auto nextStateNumber = stateNumber_;
  auto nextSequenceNumber = sequenceNumber_;
  if (stateChanged || !hasValues_) {
    if (nextStateNumber == std::numeric_limits<std::uint32_t>::max()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "GOOSE状态序号即将回绕，需要重新建立发布会话");
    }
    ++nextStateNumber;
    nextSequenceNumber = 0;
  } else {
    if (nextSequenceNumber == std::numeric_limits<std::uint32_t>::max()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "GOOSE重发序号即将回绕，需要发布新状态");
    }
    ++nextSequenceNumber;
  }
  return PublishWithSequence(values, nextStateNumber, nextSequenceNumber);
}

grpc::Status GoosePublisher::PublishWithSequence(
    std::span<const ProtocolRealtimeValue> values,
    std::uint32_t stateNumber, std::uint32_t sequenceNumber) {
  if (!IsOpen()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "GOOSE发布网卡尚未打开");
  }
  if (values.size() != currentValues_.size() || stateNumber == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "GOOSE发布序号或数据集成员数量不匹配");
  }
  auto timestampMs = timestampMs_;
  if (!hasValues_ || stateNumber != stateNumber_) {
    timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  }
  const auto status = EncodeAndSend(values, stateNumber, sequenceNumber,
                                    timestampMs);
  if (!status.ok()) {
    return status;
  }
  std::copy(values.begin(), values.end(), currentValues_.begin());
  stateNumber_ = stateNumber;
  sequenceNumber_ = sequenceNumber;
  timestampMs_ = timestampMs;
  hasValues_ = true;
  return grpc::Status::OK;
}

grpc::Status GoosePublisher::Retransmit() {
  if (!IsOpen() || !hasValues_) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "GOOSE尚未有可重发的有效状态");
  }
  if (sequenceNumber_ == std::numeric_limits<std::uint32_t>::max()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "GOOSE重发序号即将回绕，需要发布新状态");
  }
  const auto nextSequenceNumber = sequenceNumber_ + 1;
  const auto status = EncodeAndSend(currentValues_, stateNumber_,
                                    nextSequenceNumber, timestampMs_);
  if (!status.ok()) {
    return status;
  }
  sequenceNumber_ = nextSequenceNumber;
  return grpc::Status::OK;
}

grpc::Status GoosePublisher::EncodeAndSend(
    std::span<const ProtocolRealtimeValue> values,
    std::uint32_t stateNumber, std::uint32_t sequenceNumber,
    std::int64_t timestampMs) {
  GoosePublishRequest request;
  request.gocbRef = config_.gocbRef;
  request.dataSetRef = config_.dataSetRef;
  request.goId = config_.goId;
  request.timeAllowedToLiveMs = config_.timeAllowedToLiveMs;
  request.configRevision = config_.configRevision;
  request.timestampMs = timestampMs;
  request.members = config_.members;
  request.values = values;
  if (!EncodeGoosePayload(request, stateNumber, sequenceNumber,
                          config_.endpoint.appId, payload_, &payloadSize_)) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "GOOSE发布PDU超过预分配报文缓冲或编码失败");
  }
  auto status = BuildEthernetFrame(payloadSize_, &frameSize_);
  if (!status.ok()) {
    return status;
  }
  status = socket_.Send(std::span<const std::uint8_t>(frame_.data(), frameSize_));
  if (!status.ok()) {
    LOG_WARNING("IEC61850发送GOOSE报文失败: APPID={}, stNum={}, sqNum={}, 原因={}",
                config_.endpoint.appId, stateNumber, sequenceNumber,
                status.error_message());
    return status;
  }
  LOG_DEBUG("IEC61850发送GOOSE报文: APPID={}, stNum={}, sqNum={}, 字节数={}",
            config_.endpoint.appId, stateNumber, sequenceNumber, frameSize_);
  return grpc::Status::OK;
}

grpc::Status GoosePublisher::BuildEthernetFrame(
    std::size_t payloadSize, std::size_t* frameSize) noexcept {
  if (frameSize == nullptr || !destinationMac_.has_value() ||
      payloadSize > frame_.size() - 18) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "GOOSE二层发送报文超过预分配缓冲");
  }
  std::copy(destinationMac_->begin(), destinationMac_->end(), frame_.begin());
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
  frame_[offset++] = 0xb8;
  if (payloadSize > frame_.size() - offset) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "GOOSE二层发送载荷超过预分配缓冲");
  }
  std::copy_n(payload_.data(), payloadSize, frame_.data() + offset);
  *frameSize = offset + payloadSize;
  return grpc::Status::OK;
}

std::uint32_t GoosePublisher::stateNumber() const noexcept {
  return stateNumber_;
}

std::uint32_t GoosePublisher::sequenceNumber() const noexcept {
  return sequenceNumber_;
}

}  // namespace IEC61850
