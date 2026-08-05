#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850ProtocolStack.h"
#include "IEC61850RawEthernet.h"
#include "IEC61850RawProtocolStack.h"

namespace IEC61850 {

struct GoosePublisherConfig {
  ProtocolGooseEndpointPlan endpoint;
  std::string gocbRef;
  std::string dataSetRef;
  std::string goId;
  std::uint64_t configRevision = 0;
  std::uint32_t timeAllowedToLiveMs = 0;
  std::size_t memberCount = 0;
  std::vector<ProtocolGooseMemberPlan> members;
};

inline constexpr std::uint32_t kGooseInitialRetransmitIntervalMs = 1;
inline constexpr std::uint32_t kGooseDefaultMaxRetransmitIntervalMs = 1000;

// 计算下一次GOOSE重发间隔；按指数曲线递增并在上限处饱和。
std::uint32_t NextGooseRetransmitIntervalMs(
    std::uint32_t currentIntervalMs,
    std::uint32_t maxIntervalMs = kGooseDefaultMaxRetransmitIntervalMs) noexcept;

// 单个GOOSE发布端点。Publish和Retransmit必须由同一个发送线程串行调用。
class GoosePublisher {
public:
  explicit GoosePublisher(GoosePublisherConfig config);
  ~GoosePublisher();

  GoosePublisher(const GoosePublisher&) = delete;
  GoosePublisher& operator=(const GoosePublisher&) = delete;

  grpc::Status Open();
  void Close() noexcept;
  bool IsOpen() const noexcept;

  // stateChanged为true时递增stNum并将sqNum置零，否则递增sqNum。
  grpc::Status Publish(std::span<const ProtocolRealtimeValue> values,
                       bool stateChanged);
  grpc::Status PublishWithSequence(
      std::span<const ProtocolRealtimeValue> values,
      std::uint32_t stateNumber, std::uint32_t sequenceNumber);
  // 使用当前值重发，并递增sqNum。
  grpc::Status Retransmit();

  std::uint32_t stateNumber() const noexcept;
  std::uint32_t sequenceNumber() const noexcept;

private:
  grpc::Status EncodeAndSend(std::span<const ProtocolRealtimeValue> values,
                             std::uint32_t stateNumber,
                             std::uint32_t sequenceNumber,
                             std::int64_t timestampMs);
  grpc::Status BuildEthernetFrame(std::size_t payloadSize,
                                  std::size_t* frameSize) noexcept;

  GoosePublisherConfig config_;
  std::optional<std::array<std::uint8_t, 6>> destinationMac_;
  RawEthernetSocket socket_;
  std::vector<ProtocolRealtimeValue> currentValues_;
  std::array<std::uint8_t, 2048> payload_{};
  std::array<std::uint8_t, 2048> frame_{};
  std::size_t payloadSize_ = 0;
  std::size_t frameSize_ = 0;
  std::uint32_t stateNumber_ = 0;
  std::uint32_t sequenceNumber_ = 0;
  std::int64_t timestampMs_ = 0;
  bool hasValues_ = false;
};

}  // namespace IEC61850
