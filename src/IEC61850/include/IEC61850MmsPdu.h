#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <grpcpp/support/status.h>

namespace IEC61850 {

// MMS支持选项位串的有界表示；bytes只保存实际BER内容，不包含未使用位数字段。
struct MmsBitString {
  static constexpr std::size_t kMaxBytes = 32;

  std::array<std::uint8_t, kMaxBytes> bytes{};
  std::size_t size = 0;
  std::uint8_t unusedBits = 0;

  bool operator==(const MmsBitString&) const = default;
};

// MMS InitiateRequestPDU的协商参数。所有整数使用协议规定的非负范围。
struct MmsInitiateRequest {
  bool hasLocalDetailCalling = true;
  std::uint32_t localDetailCalling = 0x10000;
  bool hasProposedMaxServOutstandingCalling = true;
  std::uint16_t proposedMaxServOutstandingCalling = 10;
  bool hasProposedMaxServOutstandingCalled = true;
  std::uint16_t proposedMaxServOutstandingCalled = 10;
  bool hasProposedDataStructureNestingLevel = true;
  std::uint8_t proposedDataStructureNestingLevel = 32;
  std::uint8_t proposedVersionNumber = 1;
  MmsBitString proposedParameterSupport;
  MmsBitString proposedServiceSupport;

  bool operator==(const MmsInitiateRequest&) const = default;
};

// MMS InitiateResponsePDU的协商结果。响应字段允许按标准省略，但本项目编码器默认全部发送。
struct MmsInitiateResponse {
  bool hasLocalDetailCalled = false;
  std::uint32_t localDetailCalled = 0;
  bool hasNegotiatedMaxServOutstandingCalling = true;
  std::uint16_t negotiatedMaxServOutstandingCalling = 10;
  bool hasNegotiatedMaxServOutstandingCalled = true;
  std::uint16_t negotiatedMaxServOutstandingCalled = 10;
  bool hasNegotiatedDataStructureNestingLevel = true;
  std::uint8_t negotiatedDataStructureNestingLevel = 32;
  std::uint8_t negotiatedVersionNumber = 1;
  MmsBitString negotiatedParameterSupport;
  MmsBitString negotiatedServiceSupport;

  bool operator==(const MmsInitiateResponse&) const = default;
};

grpc::Status EncodeMmsInitiateRequest(
    const MmsInitiateRequest& request, std::span<std::uint8_t> output,
    std::size_t* outputSize);

grpc::Status DecodeMmsInitiateRequest(std::span<const std::uint8_t> input,
                                      MmsInitiateRequest* request);

grpc::Status EncodeMmsInitiateResponse(
    const MmsInitiateResponse& response, std::span<std::uint8_t> output,
    std::size_t* outputSize);

grpc::Status DecodeMmsInitiateResponse(
    std::span<const std::uint8_t> input, MmsInitiateResponse* response);

}  // namespace IEC61850
