#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

#include "IEC61850MmsPdu.h"

namespace {

IEC61850::MmsBitString ParameterSupport() {
  IEC61850::MmsBitString value;
  value.bytes[0] = 0x80;
  value.bytes[1] = 0x00;
  value.size = 2;
  value.unusedBits = 5;
  return value;
}

IEC61850::MmsBitString ServiceSupport() {
  IEC61850::MmsBitString value;
  value.bytes[0] = 0x80;
  value.size = 11;
  value.unusedBits = 3;
  return value;
}

IEC61850::MmsInitiateRequest MakeRequest() {
  IEC61850::MmsInitiateRequest request;
  request.localDetailCalling = 0x10000;
  request.proposedMaxServOutstandingCalling = 10;
  request.proposedMaxServOutstandingCalled = 10;
  request.proposedDataStructureNestingLevel = 32;
  request.proposedVersionNumber = 1;
  request.proposedParameterSupport = ParameterSupport();
  request.proposedServiceSupport = ServiceSupport();
  return request;
}

IEC61850::MmsInitiateResponse MakeResponse() {
  IEC61850::MmsInitiateResponse response;
  response.hasLocalDetailCalled = true;
  response.localDetailCalled = 0x10000;
  response.negotiatedMaxServOutstandingCalling = 300;
  response.negotiatedMaxServOutstandingCalled = 65535;
  response.negotiatedDataStructureNestingLevel = 255;
  response.negotiatedVersionNumber = 1;
  response.negotiatedParameterSupport = ParameterSupport();
  response.negotiatedServiceSupport = ServiceSupport();
  return response;
}

}  // namespace

// 验证InitiateRequest的上下文标签、整数和支持位串能够完整往返。
TEST(IEC61850MmsInitiateTest, EncodesAndDecodesRequest) {
  const auto request = MakeRequest();
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsInitiateRequest(request, encoded, &encodedSize)
                  .ok());

  constexpr std::array<std::uint8_t, 40> expected{
      0xa8, 0x26, 0x80, 0x03, 0x01, 0x00, 0x00, 0x81, 0x01, 0x0a,
      0x82, 0x01, 0x0a, 0x83, 0x01, 0x20, 0xa4, 0x16, 0x80, 0x01,
      0x01, 0x81, 0x03, 0x05, 0x80, 0x00, 0x82, 0x0c, 0x03, 0x80,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  ASSERT_EQ(encodedSize, expected.size());
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.begin() + encodedSize,
                         expected.begin()));

  IEC61850::MmsInitiateRequest decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsInitiateRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &decoded)
                  .ok());
  EXPECT_EQ(decoded, request);
}

// 验证InitiateResponse使用独立的a9外层标签，并保留协商结果。
TEST(IEC61850MmsInitiateTest, EncodesAndDecodesResponse) {
  const auto response = MakeResponse();
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsInitiateResponse(response, encoded,
                                                   &encodedSize)
                  .ok());

  constexpr std::array<std::uint8_t, 44> expected{
      0xa9, 0x2a, 0x80, 0x03, 0x01, 0x00, 0x00, 0x81, 0x02, 0x01, 0x2c,
      0x82, 0x03, 0x00, 0xff, 0xff, 0x83, 0x02, 0x00, 0xff, 0xa4, 0x16,
      0x80, 0x01, 0x01, 0x81, 0x03, 0x05, 0x80, 0x00, 0x82, 0x0c, 0x03,
      0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  ASSERT_EQ(encodedSize, expected.size());
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.begin() + encodedSize,
                         expected.begin()));

  IEC61850::MmsInitiateResponse decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsInitiateResponse(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &decoded)
                  .ok());
  EXPECT_EQ(decoded, response);
}

// 验证请求中的可选整数字段省略后仍能使用默认值完成解析。
TEST(IEC61850MmsInitiateTest, AcceptsOmittedOptionalRequestFields) {
  auto request = MakeRequest();
  request.hasLocalDetailCalling = false;
  request.hasProposedDataStructureNestingLevel = false;
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsInitiateRequest(request, encoded, &encodedSize)
                  .ok());

  IEC61850::MmsInitiateRequest decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsInitiateRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &decoded)
                  .ok());
  EXPECT_FALSE(decoded.hasLocalDetailCalling);
  EXPECT_FALSE(decoded.hasProposedDataStructureNestingLevel);
  EXPECT_EQ(decoded.proposedMaxServOutstandingCalling,
            request.proposedMaxServOutstandingCalling);
}

// 验证并发服务数是建链必需字段，不能因编解码器允许可选字段而静默放行。
TEST(IEC61850MmsInitiateTest, RejectsMissingMandatoryOutstandingFields) {
  auto request = MakeRequest();
  request.hasProposedMaxServOutstandingCalling = false;
  request.hasProposedMaxServOutstandingCalled = false;
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  EXPECT_FALSE(IEC61850::EncodeMmsInitiateRequest(request, encoded, &encodedSize)
                   .ok());

  auto response = MakeResponse();
  response.hasNegotiatedMaxServOutstandingCalling = false;
  response.hasNegotiatedMaxServOutstandingCalled = false;
  EXPECT_FALSE(IEC61850::EncodeMmsInitiateResponse(response, encoded,
                                                    &encodedSize)
                   .ok());
}

// 验证外层标签、支持位串未使用位和嵌套长度异常都会被拒绝。
TEST(IEC61850MmsInitiateTest, RejectsMalformedPdu) {
  const auto request = MakeRequest();
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsInitiateRequest(request, encoded, &encodedSize)
                  .ok());

  auto malformedTag = encoded;
  malformedTag[0] = 0xa9;
  IEC61850::MmsInitiateRequest decoded;
  EXPECT_FALSE(IEC61850::DecodeMmsInitiateRequest(
                   std::span<const std::uint8_t>(malformedTag.data(),
                                                  encodedSize),
                   &decoded)
                   .ok());

  auto malformedBits = encoded;
  malformedBits[25] = 0x01;
  EXPECT_FALSE(IEC61850::DecodeMmsInitiateRequest(
                   std::span<const std::uint8_t>(malformedBits.data(),
                                                  encodedSize),
                   &decoded)
                   .ok());

  auto truncated = encoded;
  EXPECT_FALSE(IEC61850::DecodeMmsInitiateRequest(
                   std::span<const std::uint8_t>(truncated.data(),
                                                  encodedSize - 1),
                   &decoded)
                   .ok());
}

// 验证编码器不会在输出缓冲不足时写出边界。
TEST(IEC61850MmsInitiateTest, RejectsSmallOutputBuffer) {
  const auto request = MakeRequest();
  std::array<std::uint8_t, 4> encoded{};
  std::size_t encodedSize = 99;
  EXPECT_FALSE(IEC61850::EncodeMmsInitiateRequest(request, encoded, &encodedSize)
                   .ok());
  EXPECT_EQ(encodedSize, 0u);
}
