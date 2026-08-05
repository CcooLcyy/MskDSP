#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "IEC61850MmsIsoSession.h"

namespace {

constexpr std::array<std::uint32_t, 5> kApplicationContext{
    1, 0, 9506, 2, 3};

}  // namespace

// 验证CONNECT/ACCEPT风格SPDU能够保留短用户数据并严格校验长度。
TEST(IEC61850MmsIsoSessionTest, EncodesAndDecodesConnectSpdu) {
  const std::array<std::uint8_t, 4> presentation{0xe8, 0x01, 0x60, 0x00};
  std::array<std::uint8_t, 64> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeIsoSessionConnect(
                  presentation, encoded, &encodedSize)
                  .ok());

  IEC61850::IsoSessionPduView decoded;
  ASSERT_TRUE(IEC61850::DecodeIsoSessionPdu(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &decoded)
                  .ok());
  EXPECT_EQ(decoded.type, IEC61850::IsoSessionPduType::CONNECT);
  ASSERT_EQ(decoded.userData.size(), presentation.size());
  EXPECT_TRUE(std::equal(decoded.userData.begin(), decoded.userData.end(),
                         presentation.begin()));
}

// 验证CONNECT会话参数使用长长度编码时仍能完整重组用户数据。
TEST(IEC61850MmsIsoSessionTest, EncodesAndDecodesLongSessionData) {
  std::vector<std::uint8_t> presentation(300, 0x5a);
  std::array<std::uint8_t, 512> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeIsoSessionConnect(
                  presentation, encoded, &encodedSize)
                  .ok());

  IEC61850::IsoSessionPduView decoded;
  ASSERT_TRUE(IEC61850::DecodeIsoSessionPdu(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &decoded)
                  .ok());
  EXPECT_EQ(decoded.type, IEC61850::IsoSessionPduType::CONNECT);
  EXPECT_EQ(decoded.userData.size(), presentation.size());
  EXPECT_TRUE(std::equal(decoded.userData.begin(), decoded.userData.end(),
                         presentation.begin()));
}

// 验证AARQ的MMS抽象语法用户信息能够编码并解析出原始MMS PDU。
TEST(IEC61850MmsIsoSessionTest, EncodesAndDecodesAarq) {
  const std::array<std::uint8_t, 5> initiateRequest{
      0xa8, 0x03, 0x80, 0x01, 0x01};
  std::array<std::uint8_t, 256> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsAarq(
                  kApplicationContext, initiateRequest, encoded, &encodedSize)
                  .ok());

  IEC61850::MmsAareView decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsAarq(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &decoded)
                  .ok());
  ASSERT_EQ(decoded.applicationContextOidSize, kApplicationContext.size());
  EXPECT_TRUE(std::equal(decoded.applicationContextOid.begin(),
                         decoded.applicationContextOid.begin() +
                             decoded.applicationContextOidSize,
                         kApplicationContext.begin()));
  ASSERT_EQ(decoded.mmsPdu.size(), initiateRequest.size());
  EXPECT_TRUE(std::equal(decoded.mmsPdu.begin(), decoded.mmsPdu.end(),
                         initiateRequest.begin()));
}

// 验证AARE结果和InitiateResponse用户信息能够被客户端解析。
TEST(IEC61850MmsIsoSessionTest, EncodesAndDecodesAcceptedAare) {
  const std::array<std::uint8_t, 4> initiateResponse{
      0xa9, 0x02, 0x80, 0x00};
  std::array<std::uint8_t, 256> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsAare(
                  kApplicationContext, 0, initiateResponse, encoded,
                  &encodedSize)
                  .ok());

  IEC61850::MmsAareView decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsAare(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &decoded)
                  .ok());
  EXPECT_EQ(decoded.result, 0u);
  ASSERT_EQ(decoded.mmsPdu.size(), initiateResponse.size());
  EXPECT_TRUE(std::equal(decoded.mmsPdu.begin(), decoded.mmsPdu.end(),
                         initiateResponse.begin()));
}

// 验证P-DATA-TF的Presentation Context ID和single-ASN1-type边界。
TEST(IEC61850MmsIsoSessionTest, EncodesAndDecodesPresentationData) {
  const std::array<std::uint8_t, 4> confirmedRequest{
      0xa0, 0x02, 0x02, 0x00};
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsPresentationData(
                  confirmedRequest, encoded, &encodedSize)
                  .ok());

  std::span<const std::uint8_t> decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsPresentationData(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &decoded)
                  .ok());
  ASSERT_EQ(decoded.size(), confirmedRequest.size());
  EXPECT_TRUE(std::equal(decoded.begin(), decoded.end(),
                         confirmedRequest.begin()));
}

// 验证截断的ACSE长度字段被拒绝，不把半份报文交给MMS层。
TEST(IEC61850MmsIsoSessionTest, RejectsTruncatedAcsePdu) {
  const std::array<std::uint8_t, 4> truncated{0x61, 0x05, 0xa1, 0x03};
  IEC61850::MmsAareView decoded;
  EXPECT_FALSE(IEC61850::DecodeMmsAare(truncated, &decoded).ok());
}
