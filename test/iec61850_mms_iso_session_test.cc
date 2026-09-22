#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "IEC61850MmsIsoSession.h"
#include "IEC61850MmsPdu.h"

namespace {

constexpr std::array<std::uint32_t, 5> kApplicationContext{
    1, 0, 9506, 2, 3};

// 构造与MmsSessionWorker::DefaultInitiateRequest一致的MMS InitiateRequest，
// 用于逐字节比对本模块实际发出的关联报文。
IEC61850::MmsInitiateRequest MakeInitiateRequest() {
  IEC61850::MmsInitiateRequest request;
  request.localDetailCalling = 65000;
  request.proposedMaxServOutstandingCalling = 5;
  request.proposedMaxServOutstandingCalled = 5;
  request.proposedDataStructureNestingLevel = 10;
  request.proposedParameterSupport.size = 2;
  request.proposedParameterSupport.unusedBits = 5;
  request.proposedServiceSupport.size = 11;
  request.proposedServiceSupport.unusedBits = 3;
  // 与工作器一致：NameList/Identify/Read/变量属性/Write + 文件四服务。
  request.proposedServiceSupport.bytes[0] =
      static_cast<std::uint8_t>(0x40 | 0x10 | 0x08 | 0x02 | 0x04);
  request.proposedServiceSupport.bytes[1] = 0x08;
  request.proposedServiceSupport.bytes[9] =
      static_cast<std::uint8_t>(0x80 | 0x40 | 0x20 | 0x04);
  return request;
}

// 本模块InitiateRequest的规范编码：a8外层 + a4 initRequestDetail，共41字节。
// 与参考客户端抓包的结构一致；注意localDetailCalling=65000按规范BER编码为
// 80 03 00 fd e8（带正号字节），抓包中的80 02 fd e8属非规范编码，本模块不采用。
constexpr std::array<std::uint8_t, 40> kExpectedInitiateRequest{
    0xa8, 0x26, 0x80, 0x03, 0x00, 0xfd, 0xe8, 0x81, 0x01, 0x05,
    0x82, 0x01, 0x05, 0x83, 0x01, 0x0a, 0xa4, 0x16, 0x80, 0x01,
    0x01, 0x81, 0x03, 0x05, 0x00, 0x00, 0x82, 0x0c, 0x03, 0x5e,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe4, 0x00};

// 本模块AARQ的规范编码：a1应用上下文 + be/28 EXTERNAL(indirect-reference=3) + MMS。
constexpr std::array<std::uint8_t, 64> kExpectedAarq{
    0x60, 0x3e, 0x80, 0x02, 0x07, 0x80, 0xa1, 0x07, 0x06, 0x05,
    0x28, 0xca, 0x22, 0x02, 0x03, 0xbe, 0x2f, 0x28, 0x2d, 0x02,
    0x01, 0x03, 0xa0, 0x28, 0xa8, 0x26, 0x80, 0x03, 0x00, 0xfd,
    0xe8, 0x81, 0x01, 0x05, 0x82, 0x01, 0x05, 0x83, 0x01, 0x0a,
    0xa4, 0x16, 0x80, 0x01, 0x01, 0x81, 0x03, 0x05, 0x00, 0x00,
    0x82, 0x0c, 0x03, 0x5e, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xe4, 0x00};

// 本模块表示层CP的规范编码：mode-selector + P-SEL + 上下文定义列表 + user-data(AARQ)。
constexpr std::array<std::uint8_t, 126> kExpectedPresentationCp{
    0x31, 0x7c, 0xa0, 0x03, 0x80, 0x01, 0x01, 0xa2, 0x33, 0x81,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x82, 0x05, 0x00, 0x00,
    0x00, 0x00, 0x01, 0xa4, 0x23, 0x30, 0x0f, 0x02, 0x01, 0x01,
    0x06, 0x04, 0x52, 0x01, 0x00, 0x01, 0x06, 0x04, 0x52, 0x01,
    0x00, 0x00, 0x30, 0x10, 0x02, 0x01, 0x03, 0x06, 0x05, 0x28,
    0xca, 0x22, 0x02, 0x01, 0x06, 0x04, 0x52, 0x01, 0x00, 0x01,
    0x61, 0x40, 0x60, 0x3e, 0x80, 0x02, 0x07, 0x80, 0xa1, 0x07,
    0x06, 0x05, 0x28, 0xca, 0x22, 0x02, 0x03, 0xbe, 0x2f, 0x28,
    0x2d, 0x02, 0x01, 0x03, 0xa0, 0x28, 0xa8, 0x26, 0x80, 0x03,
    0x00, 0xfd, 0xe8, 0x81, 0x01, 0x05, 0x82, 0x01, 0x05, 0x83,
    0x01, 0x0a, 0xa4, 0x16, 0x80, 0x01, 0x01, 0x81, 0x03, 0x05,
    0x00, 0x00, 0x82, 0x0c, 0x03, 0x5e, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xe4, 0x00};

// 依据本模块InitiateRequest生成AARQ；EXTERNAL必须使用indirect-reference=3。
std::vector<std::uint8_t> MakeExpectedAarq() {
  return {kExpectedAarq.begin(), kExpectedAarq.end()};
}

// 依据上面的AARQ生成表示层CP（P-SEL=00 00 00 00 01）。
std::vector<std::uint8_t> MakeExpectedPresentationCp() {
  return {kExpectedPresentationCp.begin(), kExpectedPresentationCp.end()};
}

// 用本模块编码器生成InitiateRequest字节，供AARQ用例作为用户数据输入。
std::vector<std::uint8_t> MakeExpectedInitiateRequest() {
  return {kExpectedInitiateRequest.begin(), kExpectedInitiateRequest.end()};
}

// 实测装置回应的163字节Session ACCEPT（含TPKT+COTP头），用于验证解码侧兼容性。
constexpr std::array<std::uint8_t, 163> kDeviceSessionAccept{
    0x03, 0x00, 0x00, 0xa3, 0x02, 0xf0, 0x80, 0x0e, 0x9a, 0x05, 0x06, 0x13,
    0x01, 0x00, 0x16, 0x01, 0x02, 0x14, 0x02, 0x00, 0x02, 0xc1, 0x8c, 0x31,
    0x81, 0x89, 0xa0, 0x03, 0x80, 0x01, 0x01, 0xa2, 0x81, 0x81, 0x83, 0x04,
    0x00, 0x00, 0x00, 0x01, 0xa5, 0x12, 0x30, 0x07, 0x80, 0x01, 0x00, 0x81,
    0x02, 0x51, 0x01, 0x30, 0x07, 0x80, 0x01, 0x00, 0x81, 0x02, 0x51, 0x01,
    0x88, 0x02, 0x06, 0x00, 0x61, 0x61, 0x30, 0x5f, 0x02, 0x01, 0x01, 0xa0,
    0x5a, 0x61, 0x58, 0x80, 0x02, 0x07, 0x80, 0xa1, 0x07, 0x06, 0x05, 0x28,
    0xca, 0x22, 0x02, 0x03, 0xa2, 0x03, 0x02, 0x01, 0x00, 0xa3, 0x05, 0xa1,
    0x03, 0x02, 0x01, 0x00, 0xa4, 0x07, 0x06, 0x05, 0x29, 0x01, 0x87, 0x67,
    0x01, 0xa5, 0x03, 0x02, 0x01, 0x0c, 0xbe, 0x2f, 0x28, 0x2d, 0x02, 0x01,
    0x03, 0xa0, 0x28, 0xa9, 0x26, 0x80, 0x03, 0x00, 0xfd, 0xe8, 0x81, 0x01,
    0x01, 0x82, 0x01, 0x03, 0x83, 0x01, 0x05, 0xa4, 0x16, 0x80, 0x01, 0x01,
    0x81, 0x03, 0x05, 0xf1, 0x00, 0x82, 0x0c, 0x03, 0xee, 0x1c, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x01, 0xe4, 0x18};

// 本模块Session CONNECT的golden字节（145字节）：
// 会话层SPDU(1) + 长度(1) + 连接项20字节 + c1用户数据(AARQ 64字节)。
// 连接项为 05 06 13 01 00 16 01 02 14 02 00 02 33 02 00 01 34 02 00 01。
constexpr std::array<std::uint8_t, 145> kExpectedSessionConnect{
    0x0d, 0x8f, 0x05, 0x02, 0x00, 0x02, 0x16, 0x01, 0x02, 0x33, 0x02, 0x00,
    0x01, 0x34, 0x02, 0x00, 0x01, 0xc1, 0x7e, 0x31, 0x7c, 0xa0, 0x03, 0x80,
    0x01, 0x01, 0xa2, 0x33, 0x81, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x82,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0xa4, 0x23, 0x30, 0x0f, 0x02, 0x01,
    0x01, 0x06, 0x04, 0x52, 0x01, 0x00, 0x01, 0x06, 0x04, 0x52, 0x01, 0x00,
    0x00, 0x30, 0x10, 0x02, 0x01, 0x03, 0x06, 0x05, 0x28, 0xca, 0x22, 0x02,
    0x01, 0x06, 0x04, 0x52, 0x01, 0x00, 0x01, 0x61, 0x40, 0x60, 0x3e, 0x80,
    0x02, 0x07, 0x80, 0xa1, 0x07, 0x06, 0x05, 0x28, 0xca, 0x22, 0x02, 0x03,
    0xbe, 0x2f, 0x28, 0x2d, 0x02, 0x01, 0x03, 0xa0, 0x28, 0xa8, 0x26, 0x80,
    0x03, 0x00, 0xfd, 0xe8, 0x81, 0x01, 0x05, 0x82, 0x01, 0x05, 0x83, 0x01,
    0x0a, 0xa4, 0x16, 0x80, 0x01, 0x01, 0x81, 0x03, 0x05, 0x00, 0x00, 0x82,
    0x0c, 0x03, 0x5e, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe4,
    0x00};

}  // namespace

// 验证InitiateRequest按InitiateRequestPDU结构编码：a8外层 + a4 initRequestDetail，
// 且localDetailCalling按规范BER带正号字节、参数支持位如实申报。
TEST(IEC61850MmsIsoSessionTest, EncodesInitiateRequestAsAcceptedByDevice) {
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  const auto request = MakeInitiateRequest();
  ASSERT_TRUE(
      IEC61850::EncodeMmsInitiateRequest(request, encoded, &encodedSize).ok());
  ASSERT_EQ(encodedSize, kExpectedInitiateRequest.size());
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.begin() + encodedSize,
                         kExpectedInitiateRequest.begin()));
  // 结构断言：外层a8 + a4 initRequestDetail，而不是把明细平铺成84..8a。
  EXPECT_EQ(encoded[1], 0x26);
  EXPECT_EQ(encoded[16], 0xa4);
  EXPECT_EQ(encoded[17], 0x16);
  EXPECT_EQ(encoded[2], 0x80);
  EXPECT_EQ(encoded[3], 0x03);
  // 参数支持位：81 03 05 00 00（与被接受报文一致，不虚报参数支持）。
  EXPECT_EQ(encoded[21], 0x81);
  EXPECT_EQ(encoded[22], 0x03);
  EXPECT_EQ(encoded[23], 0x05);
  EXPECT_EQ(encoded[24], 0x00);
  EXPECT_EQ(encoded[25], 0x00);
}

// 验证AARQ的user-information使用indirect-reference=3的EXTERNAL，
// 而不是抽象语法OID（实测OID形式会被装置ABORT）。
TEST(IEC61850MmsIsoSessionTest, EncodesAarqWithIndirectReferenceExternal) {
  std::array<std::uint8_t, 512> buffer{};
  std::size_t size = 0;
  const auto initiate = MakeExpectedInitiateRequest();
  ASSERT_TRUE(IEC61850::EncodeMmsAarq(kApplicationContext, initiate, buffer,
                                      &size)
                  .ok());
  const std::vector<std::uint8_t> encoded(buffer.begin(), buffer.begin() + size);
  const auto expected = MakeExpectedAarq();
  ASSERT_EQ(encoded.size(), expected.size());
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), expected.begin()));
  // be 2f 28 2d 02 01 03：EXTERNAL首字段必须是indirect-reference=3；
  // 抽象语法OID形式会是06 05 28 ca 22 02 01。
  constexpr std::array<std::uint8_t, 7> kExternalPrefix{
      0xbe, 0x2f, 0x28, 0x2d, 0x02, 0x01, 0x03};
  EXPECT_TRUE(std::equal(kExternalPrefix.begin(), kExternalPrefix.end(),
                         encoded.begin() + 15));
  // 解码回来的MMS PDU必须是同一个InitiateRequest。
  IEC61850::MmsAareView decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsAarq(encoded, &decoded).ok());
  ASSERT_EQ(decoded.mmsPdu.size(), kExpectedInitiateRequest.size());
  EXPECT_EQ(decoded.result, 0u);
}

// 验证表示层CP的BER结构：mode-selector、两个P-SEL、ACSE/MMS上下文定义
// （各自都带抽象语法与传输语法）以及承载AARQ的user-data。
TEST(IEC61850MmsIsoSessionTest, EncodesPresentationCpAsAcceptedByDevice) {
  const auto aarq = MakeExpectedAarq();
  std::array<std::uint8_t, 512> buffer{};
  std::size_t size = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsPresentationCp(
                  aarq, IEC61850::kDefaultPresentationSelector,
                  IEC61850::kDefaultPresentationSelector, buffer, &size)
                  .ok());
  const std::vector<std::uint8_t> encoded(buffer.begin(), buffer.begin() + size);
  const auto expected = MakeExpectedPresentationCp();
  ASSERT_EQ(encoded.size(), expected.size());
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), expected.begin()));
  EXPECT_EQ(encoded[0], 0x31);
  EXPECT_EQ(encoded[2], 0xa0);
  EXPECT_EQ(encoded[7], 0xa2);
  // user-data必须原样承载AARQ，服务端据此解出ACSE关联。
  ASSERT_GE(encoded.size(), aarq.size());
  EXPECT_TRUE(std::equal(aarq.begin(), aarq.end(),
                         encoded.begin() + (encoded.size() - aarq.size())));
}

// 验证CP能够解析回P-SEL与ACSE用户数据，供服务端与模拟器解封装使用。
TEST(IEC61850MmsIsoSessionTest, DecodesPresentationCp) {
  const auto cpBytes = MakeExpectedPresentationCp();
  IEC61850::PresentationCpView cp;
  ASSERT_TRUE(IEC61850::DecodeMmsPresentationCp(cpBytes, &cp).ok());
  EXPECT_EQ(cp.callingPresentationSelectorSize,
            IEC61850::kDefaultPresentationSelector.size());
  EXPECT_TRUE(std::equal(cp.callingPresentationSelector.begin(),
                         cp.callingPresentationSelector.begin() +
                             cp.callingPresentationSelectorSize,
                         IEC61850::kDefaultPresentationSelector.begin()));
  EXPECT_EQ(cp.calledPresentationSelectorSize,
            IEC61850::kDefaultPresentationSelector.size());
  EXPECT_TRUE(std::equal(cp.calledPresentationSelector.begin(),
                         cp.calledPresentationSelector.begin() +
                             cp.calledPresentationSelectorSize,
                         IEC61850::kDefaultPresentationSelector.begin()));
  const auto aarq = MakeExpectedAarq();
  ASSERT_EQ(cp.userData.size(), aarq.size());
  EXPECT_TRUE(std::equal(cp.userData.begin(), cp.userData.end(),
                         aarq.begin()));
}

// 验证Session CONNECT逐字节等于golden字节（含连接项与CP用户数据），
// 并可被自身解码器解析回CP与AARQ。
TEST(IEC61850MmsIsoSessionTest, EncodesConnectSpduWithDeviceAcceptedBytes) {
  const auto cp = MakeExpectedPresentationCp();
  std::array<std::uint8_t, 512> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(
      IEC61850::EncodeIsoSessionConnect(cp, encoded, &encodedSize).ok());
  ASSERT_EQ(encodedSize, kExpectedSessionConnect.size());
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.begin() + encodedSize,
                         kExpectedSessionConnect.begin()));
  // 连接项位于偏移2，长20字节，顺序为05/16/33/34。
  constexpr std::size_t kParameterOffset = 2;
  constexpr std::array<std::uint8_t, 20> kExpectedParameters{
      0x05, 0x06, 0x13, 0x01, 0x00, 0x16, 0x01, 0x02, 0x14, 0x02,
      0x33, 0x02, 0x00, 0x01, 0x34, 0x02, 0x00, 0x01};
  EXPECT_EQ(encoded[0], 0x0d);
  EXPECT_TRUE(std::equal(encoded.begin() + kParameterOffset,
                         encoded.begin() + kParameterOffset +
                             kExpectedParameters.size(),
                         kExpectedParameters.begin()));
  EXPECT_EQ(encoded[kParameterOffset + kExpectedParameters.size()], 0xc1);
  EXPECT_TRUE(std::equal(
      encoded.begin() + kParameterOffset + kExpectedParameters.size() + 2,
      encoded.begin() + encodedSize, cp.begin()));
  // 自解码：应还原出CP与其中的AARQ。
  IEC61850::IsoSessionPduView decoded;
  ASSERT_TRUE(IEC61850::DecodeIsoSessionPdu(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &decoded)
                  .ok());
  EXPECT_EQ(decoded.type, IEC61850::IsoSessionPduType::CONNECT);
  IEC61850::MmsAareView aarq;
  ASSERT_TRUE(IEC61850::DecodeMmsAarq(decoded.userData, &aarq).ok());
  EXPECT_EQ(aarq.mmsPdu.size(), kExpectedInitiateRequest.size());
}

// 验证解码器能解析实测装置回应的163字节Session ACCEPT（含TPKT+COTP头），
// 取出AARE与InitiateResponse；该ACCEPT的CP使用83/84形式的P-SEL与indirect-reference。
TEST(IEC61850MmsIsoSessionTest, DecodesDeviceSessionAcceptAndAare) {
  // TPKT(4字节)+COTP DT头(3字节)之后才是会话层SPDU，因此偏移为7。
  constexpr std::size_t kSessionOffset = 7;
  const std::span<const std::uint8_t> sessionPdu(
      kDeviceSessionAccept.data() + kSessionOffset,
      kDeviceSessionAccept.size() - kSessionOffset);
  IEC61850::IsoSessionPduView decoded;
  ASSERT_TRUE(IEC61850::DecodeIsoSessionPdu(sessionPdu, &decoded).ok())
      << "实测ACCEPT的连接项应按版本02校验通过";
  ASSERT_EQ(decoded.type, IEC61850::IsoSessionPduType::ACCEPT);
  IEC61850::PresentationCpView cp;
  ASSERT_TRUE(IEC61850::DecodeMmsPresentationCp(decoded.userData, &cp).ok())
      << "装置使用83/84形式的P-SEL，解码必须兼容";
  EXPECT_EQ(cp.callingPresentationSelectorSize, 4U);
  EXPECT_EQ(cp.calledPresentationSelectorSize, 4U);
  IEC61850::MmsAareView aare;
  ASSERT_TRUE(IEC61850::DecodeMmsAare(decoded.userData, &aare).ok())
      << "AARE的indirect-reference形式必须被接受";
  EXPECT_EQ(aare.result, 0u);
  ASSERT_EQ(aare.applicationContextOidSize, kApplicationContext.size());
  EXPECT_TRUE(std::equal(aare.applicationContextOid.begin(),
                         aare.applicationContextOid.begin() +
                             aare.applicationContextOidSize,
                         kApplicationContext.begin()));
  IEC61850::MmsInitiateResponse response;
  ASSERT_TRUE(IEC61850::DecodeMmsInitiateResponse(aare.mmsPdu, &response).ok());
  EXPECT_EQ(response.negotiatedDataStructureNestingLevel, 5);
  EXPECT_EQ(response.negotiatedVersionNumber, 1);
}

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

// 验证AARQ的MMS用户信息能够编码并解析出原始MMS PDU。
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

// 验证带表示层CP的AARQ（新格式客户端）能被服务端/模拟器入口解析。
TEST(IEC61850MmsIsoSessionTest, DecodesAarqWrappedInPresentationCp) {
  const auto cp = MakeExpectedPresentationCp();
  IEC61850::MmsAareView decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsAarq(cp, &decoded).ok());
  ASSERT_EQ(decoded.applicationContextOidSize, kApplicationContext.size());
  ASSERT_EQ(decoded.mmsPdu.size(), kExpectedInitiateRequest.size());
  EXPECT_TRUE(std::equal(decoded.mmsPdu.begin(), decoded.mmsPdu.end(),
                         kExpectedInitiateRequest.begin()));
}

// 验证旧的不带CP的AARQ（既有实现）仍被解码入口接受，保证模拟器联调不回归。
TEST(IEC61850MmsIsoSessionTest, DecodesLegacyAarqWithoutPresentationCp) {
  const auto aarq = MakeExpectedAarq();
  IEC61850::MmsAareView decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsAarq(aarq, &decoded).ok());
  ASSERT_EQ(decoded.mmsPdu.size(), kExpectedInitiateRequest.size());
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

// 验证连接项版本号不为02的ACCEPT会被拒绝，避免把错误协商结果当成成功关联。
TEST(IEC61850MmsIsoSessionTest, RejectsAcceptWithWrongSessionVersion) {
  auto malformed = kDeviceSessionAccept;
  // 会话层起点(7)+7 即 05 06 13 01 00 16 01 02 中的版本号字节。
  constexpr std::size_t kSessionOffset = 7;
  malformed[kSessionOffset + 7] = 0x00;
  const std::span<const std::uint8_t> sessionPdu(
      malformed.data() + kSessionOffset,
      malformed.size() - kSessionOffset);
  IEC61850::IsoSessionPduView decoded;
  EXPECT_FALSE(IEC61850::DecodeIsoSessionPdu(sessionPdu, &decoded).ok());
}

// 验证SPDU 0x0c(REFUSE)被识别为独立类型，供上层给出ACSE被拒的中文诊断。
TEST(IEC61850MmsIsoSessionTest, DecodesSessionRefuse) {
  constexpr std::array<std::uint8_t, 4> refuse{0x0c, 0x02, 0x05, 0x00};
  IEC61850::IsoSessionPduView decoded;
  ASSERT_TRUE(IEC61850::DecodeIsoSessionPdu(refuse, &decoded).ok());
  EXPECT_EQ(decoded.type, IEC61850::IsoSessionPduType::REFUSE);
  EXPECT_TRUE(decoded.userData.empty());
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

