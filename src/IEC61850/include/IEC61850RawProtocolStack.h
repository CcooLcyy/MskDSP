#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "IEC61850ProtocolStack.h"

namespace IEC61850 {

// 基于Linux AF_PACKET和自研ISO-on-TCP的IEC61850协议适配器。
std::shared_ptr<ProtocolStackAdapter> MakeRawProtocolStack();

// 解码不含Ethernet头的GOOSE二层载荷；用于报文回放和协议栈接收线程。
bool DecodeGoosePayload(
    std::span<const std::uint8_t> payload,
    const ProtocolGooseSubscriptionPlan& plan,
    IEC61850Proto::NetworkChannel channel,
    std::span<ProtocolRealtimeValue> values,
    ProtocolGooseFrameView* frame);

struct GoosePublishRequest {
  std::string_view gocbRef;
  std::string_view dataSetRef;
  std::string_view goId;
  std::uint32_t timeAllowedToLiveMs = 0;
  std::uint64_t configRevision = 0;
  // 当前GOOSE状态的事件时间；小于等于0时由编码器取当前系统时间。
  std::int64_t timestampMs = 0;
  std::span<const ProtocolGooseMemberPlan> members;
  bool simulation = false;
  bool needsCommissioning = false;
  std::span<const ProtocolRealtimeValue> values;
};

// 单次SV发布请求；values按ASDU索引再按DataSet成员顺序展平。
struct SvPublishRequest {
  std::string_view svId;
  std::uint64_t configRevision = 0;
  std::uint32_t sampleRate = 0;
  std::uint8_t sampleSynchronization = 0;
  // 当前帧每个ASDU的smpCnt，数量必须等于nofASDU。
  std::span<const std::uint16_t> sampleCounts;
  // 当前帧全部ASDU的DataSet成员值，数量必须为sampleCounts.size()*members.size()。
  std::span<const ProtocolRealtimeValue> values;
  std::span<const ProtocolSvMemberPlan> members;
  // 小于等于0时由编码器取当前系统时间，编码为IEC 61850 UtcTime。
  std::int64_t referenceTimeMs = 0;
};

// 编码不含Ethernet头的GOOSE载荷；输出缓冲由调用方提供。
bool EncodeGoosePayload(const GoosePublishRequest& request,
                        std::uint32_t stateNumber,
                        std::uint32_t sequenceNumber,
                        std::uint16_t appId,
                        std::span<std::uint8_t> output,
                        std::size_t* outputSize);

// 编码不含Ethernet头的SV二层载荷；输出缓冲由调用方提供。
bool EncodeSvPayload(const SvPublishRequest& request, std::uint16_t appId,
                     std::span<std::uint8_t> output,
                     std::size_t* outputSize);

// 解码不含Ethernet头的SV二层载荷；frames和values由调用方预分配。
bool DecodeSvPayload(
    std::span<const std::uint8_t> payload,
    const ProtocolSvStreamPlan& plan,
    IEC61850Proto::NetworkChannel channel,
    std::span<ProtocolRealtimeValue> values,
    std::span<ProtocolSvFrameView> frames,
    std::size_t* frameCount);

}  // namespace IEC61850
