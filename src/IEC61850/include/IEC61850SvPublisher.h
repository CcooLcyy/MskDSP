#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850ProtocolStack.h"
#include "IEC61850RawEthernet.h"
#include "IEC61850RawProtocolStack.h"

namespace IEC61850 {

struct SvPublisherConfig {
  ProtocolSvEndpointPlan endpoint;
  std::string svId;
  std::uint64_t configRevision = 0;
  std::uint32_t sampleRate = 0;
  std::uint32_t nofAsdu = 0;
  std::uint8_t sampleSynchronization = 0;
  std::vector<ProtocolSvMemberPlan> members;
};

// 单个SV二层发布端点。Publish和Retransmit必须由同一实时发送线程串行调用。
class SvPublisher {
public:
  explicit SvPublisher(SvPublisherConfig config);
  ~SvPublisher();

  SvPublisher(const SvPublisher&) = delete;
  SvPublisher& operator=(const SvPublisher&) = delete;

  grpc::Status Open();
  void Close() noexcept;
  bool IsOpen() const noexcept;

  // 按当前采样计数发送一帧；每个ASDU占用一个连续的smpCnt。
  grpc::Status Publish(std::span<const ProtocolRealtimeValue> values);
  // 从指定smpCnt开始发送一帧，成功后下一帧从最后一个ASDU之后继续。
  grpc::Status PublishWithSequence(
      std::span<const ProtocolRealtimeValue> values,
      std::uint16_t startingSampleCount);
  // 使用最近一帧的值和smpCnt重发，不推进采样计数。
  grpc::Status Retransmit();

  std::uint16_t nextSampleCount() const noexcept;

private:
  grpc::Status EncodeAndSend(
      std::span<const ProtocolRealtimeValue> values,
      std::span<const std::uint16_t> sampleCounts,
      std::int64_t referenceTimeMs);
  grpc::Status BuildEthernetFrame(std::size_t payloadSize,
                                  std::size_t* frameSize) noexcept;
  grpc::Status ValidateConfig() const;

  SvPublisherConfig config_;
  std::array<std::uint8_t, 6> destinationMac_{};
  bool destinationMacValid_ = false;
  RawEthernetSocket socket_;
  std::vector<ProtocolRealtimeValue> currentValues_;
  std::vector<std::uint16_t> currentSampleCounts_;
  std::vector<std::uint16_t> pendingSampleCounts_;
  std::array<std::uint8_t, 2048> payload_{};
  std::array<std::uint8_t, 2048> frame_{};
  std::size_t payloadSize_ = 0;
  std::size_t frameSize_ = 0;
  std::uint16_t nextSampleCount_ = 0;
  std::int64_t referenceTimeMs_ = 0;
  bool hasValues_ = false;
};

}  // namespace IEC61850
