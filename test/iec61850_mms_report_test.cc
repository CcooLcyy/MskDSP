#include "IEC61850MmsReport.h"

#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

void Append(std::vector<std::uint8_t>* output,
            const std::vector<std::uint8_t>& value) {
  output->insert(output->end(), value.begin(), value.end());
}

std::vector<std::uint8_t> Tlv(std::uint8_t tag,
                              std::vector<std::uint8_t> value) {
  EXPECT_LT(value.size(), 128u);
  std::vector<std::uint8_t> output;
  output.reserve(value.size() + 2);
  output.push_back(tag);
  output.push_back(static_cast<std::uint8_t>(value.size()));
  Append(&output, value);
  return output;
}

std::vector<std::uint8_t> Bytes(std::string_view value) {
  return std::vector<std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(value.data()),
      reinterpret_cast<const std::uint8_t*>(value.data() + value.size()));
}

std::vector<std::uint8_t> StringTlv(std::uint8_t tag, std::string_view value) {
  return Tlv(tag, Bytes(value));
}

std::vector<std::uint8_t> BuildReport(
    std::vector<std::uint8_t> fields,
    std::vector<std::vector<std::uint8_t>> accessResults) {
  std::vector<std::uint8_t> resultList;
  Append(&resultList, fields);
  for (const auto& result : accessResults) {
    Append(&resultList, result);
  }

  std::vector<std::uint8_t> informationReport;
  Append(&informationReport, Tlv(0xa1, Tlv(0x80, Bytes("RPT"))));
  Append(&informationReport, Tlv(0xa0, std::move(resultList)));

  return Tlv(0xa3, Tlv(0xa0, std::move(informationReport)));
}

IEC61850::MmsReportDecodePlan MakePlan() {
  IEC61850::MmsReportDecodePlan plan;
  plan.reportRef = "IED1LD0/LLN0$BR$brcb1";
  plan.reportId = "RPT";
  plan.dataSetRef = "IED1LD0/LLN0$ds1";
  plan.confRev = 3;
  plan.optionalFields.set_sequence_number(true);
  plan.optionalFields.set_report_timestamp(true);
  plan.optionalFields.set_reason_code(true);
  plan.optionalFields.set_data_set(true);
  plan.optionalFields.set_data_reference(true);
  plan.optionalFields.set_config_revision(true);
  plan.optionalFields.set_segmentation(true);
  plan.members = {
      {"IED1LD0/MMXU1.TotW.mag.f", IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX,
       false},
      {"IED1LD0/MMXU1.TotW.q", IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX,
       true},
  };
  return plan;
}

std::vector<std::uint8_t> BuildCompleteReport() {
  // OptFlds: SqNum、TimeOfEntry、Reason、DatSet、DataRef、ConfRev、分段。
  const auto optionalFields = Tlv(0x84, {0x06, 0x7c, 0xc0});
  std::vector<std::uint8_t> fields;
  Append(&fields, StringTlv(0x8a, "RPT"));
  Append(&fields, optionalFields);
  Append(&fields, Tlv(0x86, {0x02}));
  Append(&fields, Tlv(0x8c, {0x00, 0x00, 0x00, 0x00, 0x04, 0xd2}));
  Append(&fields, StringTlv(0x8a, "IED1LD0/LLN0$ds1"));
  Append(&fields, Tlv(0x86, {0x03}));
  Append(&fields, Tlv(0x86, {0x00}));
  Append(&fields, Tlv(0x83, {0xff}));
  // 两个DataSet成员均包含，BIT STRING有效位数为2。
  Append(&fields, Tlv(0x84, {0x06, 0xc0}));
  return BuildReport(
      std::move(fields),
      {StringTlv(0x8a, "IED1LD0/MMXU1$TotW$mag$f"),
       StringTlv(0x8a, "IED1LD0/MMXU1$TotW$q"), Tlv(0x83, {0xff}),
       Tlv(0x84, {0x03, 0x20, 0x00}),
       // 每个包含成员各有一个6位Reason。
       Tlv(0x84, {0x02, 0x80}), Tlv(0x84, {0x02, 0x40})});
}

}  // namespace

// 验证InformationReport的可选字段、包含位、DataRef、Reason、分段和时间戳顺序。
TEST(IEC61850MmsReportTest, DecodesCompleteInformationReport) {
  const auto plan = MakePlan();
  IEC61850::MmsReportSegment segment;
  const auto encoded = BuildCompleteReport();
  const auto status =
      IEC61850::DecodeMmsInformationReport(encoded, plan, &segment);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(segment.reportRef, plan.reportRef);
  EXPECT_EQ(segment.dataSetRef, plan.dataSetRef);
  EXPECT_EQ(segment.confRev, 3u);
  EXPECT_EQ(segment.sequenceNumber, 2u);
  EXPECT_EQ(segment.segmentNumber, 0u);
  EXPECT_TRUE(segment.moreSegmentsFollow);
  EXPECT_GT(segment.receiveTimestampMs, 0);
  ASSERT_EQ(segment.values.size(), 2u);
  EXPECT_EQ(segment.values[0].dataRef, plan.members[0].dataRef);
  EXPECT_EQ(segment.values[0].fc, plan.members[0].fc);
  ASSERT_TRUE(std::holds_alternative<bool>(segment.values[0].value));
  EXPECT_TRUE(std::get<bool>(segment.values[0].value));
  EXPECT_TRUE(
      std::holds_alternative<std::vector<std::uint8_t>>(segment.values[1].value));
  EXPECT_TRUE(segment.values[1].quality.overflow);
}

// 验证Reason bit4会被识别为GI报告标记，供会话契约完成READY判定。
TEST(IEC61850MmsReportTest, DetectsGeneralInterrogationReason) {
  auto plan = MakePlan();
  plan.optionalFields.Clear();
  plan.optionalFields.set_reason_code(true);
  plan.members = {{"IED1LD0/MMXU1.TotW.mag.f",
                   IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX, false}};

  std::vector<std::uint8_t> fields;
  Append(&fields, StringTlv(0x8a, "RPT"));
  // OptFlds仅置ReasonCode(bit3)，共10个有效位。
  Append(&fields, Tlv(0x84, {0x06, 0x10, 0x00}));
  Append(&fields, Tlv(0x84, {0x07, 0x80}));

  const auto encoded = BuildReport(std::move(fields),
                                   {Tlv(0x83, {0xff}),
                                    // Reason共6位，bit4为GI。
                                    Tlv(0x84, {0x02, 0x08})});
  IEC61850::MmsReportSegment segment;
  ASSERT_TRUE(IEC61850::DecodeMmsInformationReport(encoded, plan, &segment)
                  .ok());
  EXPECT_TRUE(segment.generalInterrogation);
}

// 验证MMS FLOAT32按0x08 format-width和IEEE-754大端值解码。
TEST(IEC61850MmsReportTest, DecodesStandardFloat32) {
  auto plan = MakePlan();
  plan.optionalFields.Clear();
  plan.members = {{"IED1LD0/MMXU1.TotW.mag.f",
                   IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX, false}};
  std::vector<std::uint8_t> fields;
  Append(&fields, StringTlv(0x8a, "RPT"));
  Append(&fields, Tlv(0x84, {0x06, 0x00, 0x00}));
  Append(&fields, Tlv(0x84, {0x07, 0x80}));
  const auto encoded = BuildReport(
      std::move(fields), {Tlv(0x87, {0x08, 0x3f, 0xc0, 0x00, 0x00})});
  IEC61850::MmsReportSegment segment;

  ASSERT_TRUE(IEC61850::DecodeMmsInformationReport(encoded, plan, &segment)
                  .ok());
  ASSERT_EQ(segment.values.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<double>(segment.values[0].value));
  EXPECT_DOUBLE_EQ(std::get<double>(segment.values[0].value), 1.5);
}

// 验证MMS FLOAT64按0x0B format-width和IEEE-754大端值解码。
TEST(IEC61850MmsReportTest, DecodesStandardFloat64) {
  auto plan = MakePlan();
  plan.optionalFields.Clear();
  plan.members = {{"IED1LD0/MMXU1.TotW.mag.f",
                   IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX, false}};
  std::vector<std::uint8_t> fields;
  Append(&fields, StringTlv(0x8a, "RPT"));
  Append(&fields, Tlv(0x84, {0x06, 0x00, 0x00}));
  Append(&fields, Tlv(0x84, {0x07, 0x80}));
  const auto encoded = BuildReport(
      std::move(fields),
      {Tlv(0x87, {0x0b, 0x3f, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})});
  IEC61850::MmsReportSegment segment;

  ASSERT_TRUE(IEC61850::DecodeMmsInformationReport(encoded, plan, &segment)
                  .ok());
  ASSERT_EQ(segment.values.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<double>(segment.values[0].value));
  EXPECT_DOUBLE_EQ(std::get<double>(segment.values[0].value), 1.25);
}

// 验证MMS浮点format-width和IEEE-754载荷长度不匹配时拒绝报告。
TEST(IEC61850MmsReportTest, RejectsMismatchedFloatingPointFormatWidth) {
  auto plan = MakePlan();
  plan.optionalFields.Clear();
  plan.members = {{"IED1LD0/MMXU1.TotW.mag.f",
                   IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX, false}};
  std::vector<std::uint8_t> fields;
  Append(&fields, StringTlv(0x8a, "RPT"));
  Append(&fields, Tlv(0x84, {0x06, 0x00, 0x00}));
  Append(&fields, Tlv(0x84, {0x07, 0x80}));
  const auto encoded = BuildReport(
      std::move(fields),
      {Tlv(0x87, {0x08, 0x3f, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})});
  IEC61850::MmsReportSegment segment;

  EXPECT_FALSE(IEC61850::DecodeMmsInformationReport(encoded, plan, &segment)
                   .ok());
}

// 验证错误的RptID、OptFlds或成员结构不能被当作有效报告交付。
TEST(IEC61850MmsReportTest, RejectsMismatchedReportIdentity) {
  auto encoded = BuildCompleteReport();
  IEC61850::MmsReportDecodePlan plan = MakePlan();
  plan.reportId = "OTHER";
  IEC61850::MmsReportSegment segment;
  EXPECT_FALSE(IEC61850::DecodeMmsInformationReport(encoded, plan, &segment)
                   .ok());

  plan = MakePlan();
  plan.optionalFields.set_segmentation(false);
  EXPECT_FALSE(IEC61850::DecodeMmsInformationReport(encoded, plan, &segment)
                   .ok());
}

// 验证数组和结构Data按有界递归树保留层级，不静默转字符串或丢弃成员。
TEST(IEC61850MmsReportTest, DecodesBoundedStructuredData) {
  auto plan = MakePlan();
  plan.optionalFields.Clear();
  plan.members = {{"IED1LD0/MMXU1.TotW.mag.f",
                   IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX, false}};
  std::vector<std::uint8_t> fields;
  Append(&fields, StringTlv(0x8a, "RPT"));
  Append(&fields, Tlv(0x84, {0x06, 0x00, 0x00}));
  Append(&fields, Tlv(0x84, {0x07, 0x80}));
  const auto encoded =
      BuildReport(std::move(fields),
                  {Tlv(0xa2, {0x83, 0x01, 0xff,
                               0xa1, 0x06, 0x85, 0x01, 0x01,
                               0x85, 0x01, 0x02})});
  IEC61850::MmsReportSegment segment;
  ASSERT_TRUE(IEC61850::DecodeMmsInformationReport(encoded, plan, &segment)
                  .ok());
  ASSERT_EQ(segment.values.size(), 1u);
  const auto* structure = std::get_if<
      std::shared_ptr<IEC61850::MmsCompositeValue>>(&segment.values[0].value);
  ASSERT_NE(structure, nullptr);
  ASSERT_NE(*structure, nullptr);
  EXPECT_EQ((*structure)->kind,
            IEC61850::MmsCompositeValue::Kind::STRUCTURE);
  ASSERT_EQ((*structure)->elements.size(), 2u);
  EXPECT_TRUE(std::get<bool>((*structure)->elements[0]));
  const auto* array = std::get_if<std::shared_ptr<IEC61850::MmsCompositeValue>>(
      &(*structure)->elements[1]);
  ASSERT_NE(array, nullptr);
  ASSERT_NE(*array, nullptr);
  EXPECT_EQ((*array)->kind, IEC61850::MmsCompositeValue::Kind::ARRAY);
  ASSERT_EQ((*array)->elements.size(), 2u);
  EXPECT_EQ(std::get<std::int64_t>((*array)->elements[0]), 1);
  EXPECT_EQ(std::get<std::int64_t>((*array)->elements[1]), 2);
}
