#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

#include "IEC61850MmsBer.h"

namespace {

// 验证：长格式长度和嵌套TLV能够被完整读取。
TEST(IEC61850MmsBerTest, ReadsLongFormLengthAndNestedTlv) {
  std::array<std::uint8_t, 260> bytes{};
  IEC61850::BerWriter writer(bytes);
  std::array<std::uint8_t, 128> value{};
  ASSERT_TRUE(writer.Tlv(0xa0, value));

  std::size_t offset = 0;
  IEC61850::BerTlvView outer;
  ASSERT_TRUE(IEC61850::ReadBerTlv(writer.written(), &offset, &outer).ok());
  EXPECT_EQ(outer.tag, 0xa0);
  EXPECT_EQ(outer.value.size(), value.size());
  EXPECT_EQ(offset, writer.size());
}

// 验证：有符号和无符号整数的规范编码可以往返转换。
TEST(IEC61850MmsBerTest, RoundTripsSignedAndUnsignedIntegers) {
  std::array<std::uint8_t, 64> bytes{};
  IEC61850::BerWriter writer(bytes);
  ASSERT_TRUE(writer.Signed(0x02, -129));
  ASSERT_TRUE(writer.Unsigned(0x02, 0x80));

  std::size_t offset = 0;
  IEC61850::BerTlvView tlv;
  ASSERT_TRUE(IEC61850::ReadBerTlv(writer.written(), &offset, &tlv).ok());
  std::int64_t signedValue = 0;
  ASSERT_TRUE(IEC61850::ReadBerSigned(tlv.value, &signedValue).ok());
  EXPECT_EQ(signedValue, -129);
  ASSERT_TRUE(IEC61850::ReadBerTlv(writer.written(), &offset, &tlv).ok());
  std::uint64_t unsignedValue = 0;
  ASSERT_TRUE(IEC61850::ReadBerUnsigned(tlv.value, &unsignedValue).ok());
  EXPECT_EQ(unsignedValue, 0x80u);
}

// 验证无符号整数在128、255和65535边界使用正号补零后仍可解码。
TEST(IEC61850MmsBerTest, RoundTripsUnsignedSignOctetBoundaries) {
  constexpr std::array<std::uint64_t, 3> values{128, 255, 65535};
  for (const auto expected : values) {
    std::array<std::uint8_t, 32> bytes{};
    IEC61850::BerWriter writer(bytes);
    ASSERT_TRUE(writer.Unsigned(0x02, expected));

    std::size_t offset = 0;
    IEC61850::BerTlvView tlv;
    ASSERT_TRUE(IEC61850::ReadBerTlv(writer.written(), &offset, &tlv).ok());
    std::uint64_t decoded = 0;
    ASSERT_TRUE(IEC61850::ReadBerUnsigned(tlv.value, &decoded).ok());
    EXPECT_EQ(decoded, expected);
  }
}

// 验证：OID和布尔值采用标准BER编码并可被读取。
TEST(IEC61850MmsBerTest, RoundTripsOidAndBoolean) {
  constexpr std::array<std::uint32_t, 7> expected{1, 0, 9506, 2, 1, 1, 1};
  std::array<std::uint8_t, 64> bytes{};
  IEC61850::BerWriter writer(bytes);
  ASSERT_TRUE(writer.Oid(0x06, expected));
  ASSERT_TRUE(writer.Boolean(0x01, true));

  std::size_t offset = 0;
  IEC61850::BerTlvView tlv;
  ASSERT_TRUE(IEC61850::ReadBerTlv(writer.written(), &offset, &tlv).ok());
  std::array<std::uint32_t, 8> actual{};
  std::size_t actualCount = 0;
  ASSERT_TRUE(IEC61850::ReadBerOid(tlv.value, actual, &actualCount).ok());
  EXPECT_EQ(actualCount, expected.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), actual.begin()));
  ASSERT_TRUE(IEC61850::ReadBerTlv(writer.written(), &offset, &tlv).ok());
  bool booleanValue = false;
  ASSERT_TRUE(IEC61850::ReadBerBoolean(tlv.value, &booleanValue).ok());
  EXPECT_TRUE(booleanValue);
}

// 验证File服务使用的高标签号能够按BER高标签格式编码和解码。
TEST(IEC61850MmsBerTest, RoundTripsHighTagNumber) {
  std::array<std::uint8_t, 32> bytes{};
  IEC61850::BerWriter writer(bytes);
  constexpr std::uint32_t fileOpenTag = (0xbfu << 24) | 72u;
  constexpr std::array<std::uint8_t, 2> value{0x01, 0x02};
  ASSERT_TRUE(writer.Tlv(fileOpenTag, value));
  ASSERT_EQ(writer.written().size(), 5u);
  EXPECT_EQ(writer.written()[0], 0xbfu);
  EXPECT_EQ(writer.written()[1], 0x48u);

  std::size_t offset = 0;
  IEC61850::BerTlvView decoded;
  ASSERT_TRUE(IEC61850::ReadBerTlv(writer.written(), &offset, &decoded).ok());
  EXPECT_EQ(decoded.identifierOctet, 0xbfu);
  EXPECT_EQ(decoded.tagNumber, 72u);
  ASSERT_EQ(decoded.value.size(), value.size());
  EXPECT_TRUE(std::equal(decoded.value.begin(), decoded.value.end(),
                         value.begin()));
}

// 验证：无限长度、截断值和非规范整数都会被拒绝且不产生部分结果。
TEST(IEC61850MmsBerTest, RejectsMalformedValues) {
  constexpr std::array<std::uint8_t, 2> indefinite{0x30, 0x80};
  constexpr std::array<std::uint8_t, 3> truncated{0x04, 0x02, 0x01};
  constexpr std::array<std::uint8_t, 4> redundantInteger{0x02, 0x02, 0x00,
                                                          0x01};
  std::size_t offset = 0;
  IEC61850::BerTlvView tlv;
  EXPECT_FALSE(IEC61850::ReadBerTlv(indefinite, &offset, &tlv).ok());
  offset = 0;
  EXPECT_FALSE(IEC61850::ReadBerTlv(truncated, &offset, &tlv).ok());
  offset = 0;
  ASSERT_TRUE(IEC61850::ReadBerTlv(redundantInteger, &offset, &tlv).ok());
  std::int64_t value = 0;
  EXPECT_FALSE(IEC61850::ReadBerSigned(tlv.value, &value).ok());
}

}  // namespace
