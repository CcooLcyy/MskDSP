#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "IEC61850MmsTransport.h"

namespace {

// 验证：COTP连接请求包含合法TPKT长度、连接请求类型和源引用。
TEST(IEC61850MmsTransportTest, EncodesConnectionRequest) {
  std::array<std::uint8_t, 64> output{};
  std::size_t outputSize = 0;

  ASSERT_TRUE(IEC61850::EncodeCotpConnectionRequest(output, &outputSize, 0x1234)
                  .ok());
  ASSERT_EQ(outputSize, 22u);
  EXPECT_EQ(output[0], 0x03);
  EXPECT_EQ(output[1], 0x00);
  EXPECT_EQ(output[2], 0x00);
  EXPECT_EQ(output[3], outputSize);
  EXPECT_EQ(output[4], 0x11);
  EXPECT_EQ(output[5], 0xe0);
  EXPECT_EQ(output[8], 0x12);
  EXPECT_EQ(output[9], 0x34);
}

// 验证：合法服务端COTP连接确认可以通过校验，其他TPDU会被拒绝。
TEST(IEC61850MmsTransportTest, ValidatesConnectionConfirm) {
  constexpr std::array<std::uint8_t, 13> confirm{
      0x03, 0x00, 0x00, 0x0d, 0x08, 0xd0, 0x00, 0x01,
      0x00, 0x01, 0x00, 0xc0, 0x01};

  EXPECT_TRUE(IEC61850::ValidateCotpConnectionConfirm(confirm).ok());

  auto invalid = confirm;
  invalid[5] = 0xf0;
  EXPECT_FALSE(IEC61850::ValidateCotpConnectionConfirm(invalid).ok());
}

// 验证：MMS载荷可以封装和解封装为单个TPKT/COTP数据帧。
TEST(IEC61850MmsTransportTest, EncodesAndDecodesData) {
  constexpr std::array<std::uint8_t, 5> input{0x60, 0x03, 0x01, 0x02, 0x03};
  std::array<std::uint8_t, 64> frame{};
  std::size_t frameSize = 0;
  ASSERT_TRUE(IEC61850::EncodeCotpData(input, frame, &frameSize).ok());
  ASSERT_EQ(frameSize, input.size() + 7);

  std::array<std::uint8_t, 64> output{};
  std::size_t outputSize = 0;
  ASSERT_TRUE(IEC61850::DecodeCotpData(
                  std::span<const std::uint8_t>(frame.data(), frameSize),
                  output, &outputSize)
                  .ok());
  EXPECT_EQ(outputSize, input.size());
  EXPECT_TRUE(std::equal(input.begin(), input.end(), output.begin()));
}

// 验证：TPKT版本、长度和COTP数据头任一异常都会拒绝整帧。
TEST(IEC61850MmsTransportTest, RejectsMalformedDataFrame) {
  constexpr std::array<std::uint8_t, 8> frame{
      0x03, 0x00, 0x00, 0x08, 0x02, 0xf0, 0x81, 0x00};
  std::array<std::uint8_t, 16> payload{};
  std::size_t payloadSize = 99;

  EXPECT_FALSE(IEC61850::DecodeCotpData(frame, payload, &payloadSize).ok());
  EXPECT_EQ(payloadSize, 0u);
}

// 验证超过单个TPDU上限的MMS载荷会按EOT分成多个连续数据段。
TEST(IEC61850MmsTransportTest, EncodesAndDecodesDataSegments) {
  std::vector<std::uint8_t> input(2500, 0x5a);
  std::vector<std::vector<std::uint8_t>> frames;
  ASSERT_TRUE(IEC61850::EncodeCotpDataSegments(input, &frames).ok());
  ASSERT_GT(frames.size(), 1u);

  std::vector<std::uint8_t> reassembled;
  std::array<std::uint8_t, 2048> segmentPayload{};
  for (std::size_t index = 0; index < frames.size(); ++index) {
    std::size_t payloadSize = 0;
    bool endOfTransport = false;
    ASSERT_TRUE(IEC61850::DecodeCotpDataFrame(
                    frames[index], segmentPayload, &payloadSize,
                    &endOfTransport)
                    .ok());
    EXPECT_EQ(endOfTransport, index + 1 == frames.size());
    reassembled.insert(reassembled.end(), segmentPayload.begin(),
                       segmentPayload.begin() + payloadSize);
  }
  EXPECT_EQ(reassembled, input);
}

// 验证COTP分段编码拒绝空载荷和空输出参数。
TEST(IEC61850MmsTransportTest, RejectsInvalidSegmentArguments) {
  std::vector<std::vector<std::uint8_t>> frames;
  EXPECT_FALSE(IEC61850::EncodeCotpDataSegments(
                   std::span<const std::uint8_t>{}, &frames)
                   .ok());
  EXPECT_FALSE(IEC61850::EncodeCotpDataSegments(
                   std::array<std::uint8_t, 1>{0x01}, nullptr)
                   .ok());
}

}  // namespace
