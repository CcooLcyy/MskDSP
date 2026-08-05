#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "IEC61850MmsRcb.h"

namespace {

void AppendField(std::vector<std::uint8_t>* output, std::uint8_t tag,
                 std::initializer_list<std::uint8_t> value) {
  output->push_back(tag);
  output->push_back(static_cast<std::uint8_t>(value.size()));
  output->insert(output->end(), value.begin(), value.end());
}

std::vector<std::uint8_t> BuildRcb(bool buffered) {
  std::vector<std::uint8_t> fields;
  AppendField(&fields, 0x8a, {'r', '1'});
  AppendField(&fields, 0x83, {0xff});
  if (!buffered) {
    AppendField(&fields, 0x83, {0x00});
  }
  AppendField(&fields, 0x8a, {'L', 'D', '0', '/', 'd', 's'});
  AppendField(&fields, 0x86, {0x07});
  // BIT STRING(10): bit0保留，bit1、bit3和bit9置位。
  AppendField(&fields, 0x84, {0x06, 0x50, 0x40});
  AppendField(&fields, 0x86, {0x03, 0xe8});
  AppendField(&fields, 0x86, {0x02});
  // BIT STRING(6): dchg和integrity置位。
  AppendField(&fields, 0x84, {0x02, 0x48});
  // 60000的最高有效位为1，Unsigned必须带规范正号八位组。
  AppendField(&fields, 0x86, {0x00, 0xea, 0x60});
  AppendField(&fields, 0x83, {0x00});
  std::vector<std::uint8_t> encoded{0xa2,
                                    static_cast<std::uint8_t>(fields.size())};
  encoded.insert(encoded.end(), fields.begin(), fields.end());
  return encoded;
}

// 验证BRCB根对象的Data.structure字段顺序、位串位序和GI不会覆盖RptEna。
TEST(IEC61850MmsRcbTest, DecodesBufferedReportControl) {
  const auto encoded = BuildRcb(true);
  IEC61850::MmsDirectoryReportControl result;
  ASSERT_TRUE(IEC61850::DecodeMmsRcbData(encoded, true, &result).ok());
  EXPECT_EQ(result.reportId, "r1");
  EXPECT_TRUE(result.reportEnabled);
  EXPECT_EQ(result.dataSetRef, "LD0/ds");
  EXPECT_EQ(result.configRevision, 7u);
  EXPECT_EQ(result.bufferTimeMs, 1000u);
  EXPECT_EQ(result.integrityPeriodMs, 60000u);
  EXPECT_TRUE(result.optionalFields.sequence_number());
  EXPECT_TRUE(result.optionalFields.reason_code());
  EXPECT_TRUE(result.optionalFields.segmentation());
  EXPECT_TRUE(result.triggerOptions.data_change());
  EXPECT_TRUE(result.triggerOptions.integrity());
  EXPECT_FALSE(result.triggerOptions.general_interrogation());
}

// 验证URCB在RptEna后包含Resv字段时仍能按不同布局解码。
TEST(IEC61850MmsRcbTest, DecodesUnbufferedReportControl) {
  const auto encoded = BuildRcb(false);
  IEC61850::MmsDirectoryReportControl result;
  ASSERT_TRUE(IEC61850::DecodeMmsRcbData(encoded, false, &result).ok());
  EXPECT_TRUE(result.reportEnabled);
  EXPECT_EQ(result.dataSetRef, "LD0/ds");
  EXPECT_EQ(result.configRevision, 7u);
}

// 验证根对象标签、BIT STRING保留位和未知尾部字段不会被宽松接受。
TEST(IEC61850MmsRcbTest, RejectsMalformedStructureAndBitStrings) {
  auto malformed = BuildRcb(true);
  malformed[0] = 0xa1;
  IEC61850::MmsDirectoryReportControl result;
  EXPECT_FALSE(IEC61850::DecodeMmsRcbData(malformed, true, &result).ok());

  malformed = BuildRcb(true);
  malformed[24] = 0x41;
  EXPECT_FALSE(IEC61850::DecodeMmsRcbData(malformed, true, &result).ok());

  malformed = BuildRcb(true);
  malformed[22] = static_cast<std::uint8_t>(malformed[22] | 0x80);
  EXPECT_FALSE(IEC61850::DecodeMmsRcbData(malformed, true, &result).ok());

  malformed = BuildRcb(true);
  malformed.push_back(0x81);
  malformed.push_back(0x00);
  malformed[1] = static_cast<std::uint8_t>(malformed.size() - 2);
  EXPECT_FALSE(IEC61850::DecodeMmsRcbData(malformed, true, &result).ok());
}

// 验证BufTm和IntgPd超过运行时宽度时被拒绝。
TEST(IEC61850MmsRcbTest, RejectsUnsignedValuesOutsideRuntimeRange) {
  auto malformed = BuildRcb(true);
  // BufTm字段位于OptFlds之后，替换为超过uint32的五字节大整数。
  malformed[26] = 0x05;
  malformed[27] = 0x01;
  malformed.erase(malformed.begin() + 28);
  malformed.insert(malformed.begin() + 28, {0x00, 0x00, 0x00, 0x00});
  malformed[1] = static_cast<std::uint8_t>(malformed.size() - 2);
  IEC61850::MmsDirectoryReportControl result;
  EXPECT_FALSE(IEC61850::DecodeMmsRcbData(malformed, true, &result).ok());
}

// 验证RCB三阶段Write请求的字段顺序、MMS对象引用和Data标签。
TEST(IEC61850MmsRcbTest, BuildsRcbWritePhases) {
  IEC61850::MmsRcbActivationRequest activation;
  activation.rcbRef = "IED1LD0/LLN0$BR$brcb1";
  activation.dataSetRef = "IED1LD0/LLN0$Events";
  activation.reportId = "report-1";
  activation.bufferTimeMs = 25;
  activation.integrityPeriodMs = 60000;
  activation.optionalFields.set_sequence_number(true);
  activation.optionalFields.set_reason_code(true);
  activation.optionalFields.set_segmentation(true);
  activation.triggerOptions.set_data_change(true);
  activation.triggerOptions.set_integrity(true);

  IEC61850::MmsWriteRequest request;
  ASSERT_TRUE(IEC61850::BuildMmsRcbWriteRequest(
                  activation, IEC61850::MmsRcbWritePhase::DISABLE, &request)
                  .ok());
  ASSERT_EQ(request.items.size(), 1u);
  EXPECT_EQ(request.items[0].variable.domain, "IED1LD0");
  EXPECT_EQ(request.items[0].variable.identifier, "LLN0$BR$brcb1$RptEna");
  EXPECT_EQ(request.items[0].encodedData,
            (std::vector<std::uint8_t>{0x83, 0x01, 0x00}));

  ASSERT_TRUE(IEC61850::BuildMmsRcbWriteRequest(
                  activation, IEC61850::MmsRcbWritePhase::CONFIGURE, &request)
                  .ok());
  ASSERT_EQ(request.items.size(), 6u);
  const std::vector<std::string> expectedMembers{
      "LLN0$BR$brcb1$RptID",  "LLN0$BR$brcb1$DatSet",
      "LLN0$BR$brcb1$OptFlds", "LLN0$BR$brcb1$BufTm",
      "LLN0$BR$brcb1$TrgOps", "LLN0$BR$brcb1$IntgPd"};
  for (std::size_t index = 0; index < expectedMembers.size(); ++index) {
    EXPECT_EQ(request.items[index].variable.identifier, expectedMembers[index]);
  }
  EXPECT_EQ(request.items[0].encodedData,
            (std::vector<std::uint8_t>{0x8a, 0x08, 'r', 'e', 'p', 'o', 'r',
                                       't', '-', '1'}));
  EXPECT_EQ(request.items[1].encodedData,
            (std::vector<std::uint8_t>{0x8a, 0x13, 'I', 'E', 'D', '1', 'L',
                                       'D', '0', '/', 'L', 'L', 'N', '0', '$',
                                       'E', 'v', 'e', 'n', 't', 's'}));
  EXPECT_EQ(request.items[2].encodedData,
            (std::vector<std::uint8_t>{0x84, 0x03, 0x06, 0x50, 0x40}));
  EXPECT_EQ(request.items[3].encodedData,
            (std::vector<std::uint8_t>{0x86, 0x01, 0x19}));
  EXPECT_EQ(request.items[4].encodedData,
            (std::vector<std::uint8_t>{0x84, 0x02, 0x02, 0x48}));
  EXPECT_EQ(request.items[5].encodedData,
            (std::vector<std::uint8_t>{0x86, 0x03, 0x00, 0xea, 0x60}));

  ASSERT_TRUE(IEC61850::BuildMmsRcbWriteRequest(
                  activation, IEC61850::MmsRcbWritePhase::ENABLE, &request)
                  .ok());
  ASSERT_EQ(request.items.size(), 1u);
  EXPECT_EQ(request.items[0].variable.identifier, "LLN0$BR$brcb1$RptEna");
  EXPECT_EQ(request.items[0].encodedData,
            (std::vector<std::uint8_t>{0x83, 0x01, 0xff}));
}

// 验证GI使用独立的$GI=true Write，不混入RCB配置字段列表。
TEST(IEC61850MmsRcbTest, BuildsIndependentGeneralInterrogationWrite) {
  IEC61850::MmsRcbActivationRequest activation;
  activation.rcbRef = "IED1LD0/LLN0$BR$brcb1";
  activation.generalInterrogation = true;

  IEC61850::MmsWriteRequest request;
  ASSERT_TRUE(IEC61850::BuildMmsRcbGeneralInterrogationRequest(activation,
                                                               &request)
                  .ok());
  ASSERT_EQ(request.items.size(), 1u);
  EXPECT_EQ(request.items[0].variable.identifier, "LLN0$BR$brcb1$GI");
  EXPECT_EQ(request.items[0].encodedData,
            (std::vector<std::uint8_t>{0x83, 0x01, 0xff}));
}

// 验证RCB写入拒绝空引用、非法阶段和不完整配置。
TEST(IEC61850MmsRcbTest, RejectsInvalidRcbWriteRequest) {
  IEC61850::MmsRcbActivationRequest activation;
  IEC61850::MmsWriteRequest request;
  EXPECT_FALSE(IEC61850::BuildMmsRcbWriteRequest(
                   activation, IEC61850::MmsRcbWritePhase::DISABLE, &request)
                   .ok());

  activation.rcbRef = "IED1LD0/LLN0$BR$brcb1";
  EXPECT_FALSE(IEC61850::BuildMmsRcbWriteRequest(
                   activation, IEC61850::MmsRcbWritePhase::CONFIGURE, &request)
                   .ok());
  EXPECT_FALSE(IEC61850::BuildMmsRcbWriteRequest(
                   activation, static_cast<IEC61850::MmsRcbWritePhase>(99),
                   &request)
                   .ok());
}

}  // namespace
