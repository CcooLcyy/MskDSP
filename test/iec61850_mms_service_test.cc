#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "IEC61850MmsBer.h"
#include "IEC61850MmsService.h"

namespace {

// 验证GetNameList请求使用Confirmed-Request、Domain对象类和VMD范围的标准标签。
TEST(IEC61850MmsServiceTest, EncodesGetNameListRequest) {
  IEC61850::MmsGetNameListRequest request;
  request.objectClass = IEC61850::MmsObjectClass::DOMAIN;
  request.scope.type = IEC61850::MmsObjectScopeType::VMD_SPECIFIC;

  std::array<std::uint8_t, 64> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsGetNameListRequest(
                  7, request, encoded, &encodedSize)
                  .ok());
  constexpr std::array<std::uint8_t, 16> expected{
      0xa0, 0x0e, 0x02, 0x01, 0x07, 0xa1, 0x09, 0xa0, 0x03,
      0x80, 0x01, 0x08, 0xa1, 0x02, 0x80, 0x00};
  ASSERT_EQ(encodedSize, expected.size());
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.begin() + encodedSize,
                         expected.begin()));
}

// 验证GetNameList请求的对象范围和continueAfter字段能够完整往返。
TEST(IEC61850MmsServiceTest, DecodesGetNameListRequest) {
  IEC61850::MmsGetNameListRequest request;
  request.objectClass = IEC61850::MmsObjectClass::NAMED_VARIABLE;
  request.scope.type = IEC61850::MmsObjectScopeType::DOMAIN_SPECIFIC;
  request.scope.domain = "LD0";
  request.continueAfter = "LLN0";

  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsGetNameListRequest(
                  11, request, encoded, &encodedSize)
                  .ok());

  std::uint32_t invokeId = 0;
  IEC61850::MmsGetNameListRequest decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsGetNameListRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &decoded)
                  .ok());
  EXPECT_EQ(invokeId, 11u);
  EXPECT_EQ(decoded.objectClass, request.objectClass);
  EXPECT_EQ(decoded.scope.type, request.scope.type);
  EXPECT_EQ(decoded.scope.domain, request.scope.domain);
  ASSERT_TRUE(decoded.continueAfter.has_value());
  EXPECT_EQ(*decoded.continueAfter, *request.continueAfter);
}

// 验证GetNameList响应能解析有序Identifier和moreFollows标志，并核对invokeID。
TEST(IEC61850MmsServiceTest, DecodesGetNameListResponse) {
  constexpr std::array<std::uint8_t, 22> response{
      0xa1, 0x14, 0x02, 0x01, 0x07, 0xa1, 0x0f, 0xa0, 0x0a,
      0x1a, 0x03, 0x4c, 0x44, 0x30, 0x1a, 0x03, 0x4c, 0x44,
      0x31, 0x81, 0x01, 0x00};
  IEC61850::MmsGetNameListResponse decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsGetNameListResponse(response, 7, &decoded)
                  .ok());
  ASSERT_EQ(decoded.identifiers.size(), 2u);
  EXPECT_EQ(decoded.identifiers[0], "LD0");
  EXPECT_EQ(decoded.identifiers[1], "LD1");
  EXPECT_FALSE(decoded.moreFollows);
}

// 验证GetNameList响应缺少必需的moreFollows字段时不会被当作分页响应接受。
TEST(IEC61850MmsServiceTest, RejectsGetNameListResponseWithoutMoreFollows) {
  constexpr std::array<std::uint8_t, 14> response{
      0xa1, 0x0c, 0x02, 0x01, 0x07, 0xa1, 0x07, 0xa0, 0x05,
      0x1a, 0x03, 'L',  'D',  '0'};
  IEC61850::MmsGetNameListResponse decoded;
  EXPECT_FALSE(IEC61850::DecodeMmsGetNameListResponse(response, 7, &decoded)
                   .ok());
  EXPECT_TRUE(decoded.identifiers.empty());
}

// 验证响应invokeID不匹配时不会把其他请求的结果交给当前请求。
TEST(IEC61850MmsServiceTest, RejectsMismatchedResponseInvokeId) {
  constexpr std::array<std::uint8_t, 22> response{
      0xa1, 0x14, 0x02, 0x01, 0x07, 0xa1, 0x0f, 0xa0, 0x0a,
      0x1a, 0x03, 0x4c, 0x44, 0x30, 0x1a, 0x03, 0x4c, 0x44,
      0x31, 0x81, 0x01, 0x00};
  IEC61850::MmsGetNameListResponse decoded;
  EXPECT_FALSE(IEC61850::DecodeMmsGetNameListResponse(response, 8, &decoded)
                   .ok());
}

// 验证Confirmed-ErrorPDU能够解析invokeID、错误类、错误码和附加描述。
TEST(IEC61850MmsServiceTest, DecodesConfirmedErrorPdu) {
  constexpr std::array<std::uint8_t, 21> response{
      0xa2, 0x13, 0x02, 0x01, 0x07, 0x30, 0x0e, 0x87, 0x01, 0x03,
      0x81, 0x01, 0x2a, 0x82, 0x06, 'd',  'e',  'n',  'i',  'e',  'd'};
  IEC61850::MmsConfirmedErrorPduView decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsConfirmedError(response, &decoded).ok());
  EXPECT_EQ(decoded.invokeId, 7u);
  EXPECT_EQ(decoded.errorClass, 7u);
  EXPECT_EQ(decoded.errorCode, 3);
  ASSERT_TRUE(decoded.additionalCode.has_value());
  EXPECT_EQ(*decoded.additionalCode, 42);
  EXPECT_EQ(decoded.additionalDescription, "denied");
}

// 验证Confirmed-ErrorPDU缺少错误类或包含非法字段时被拒绝。
TEST(IEC61850MmsServiceTest, RejectsMalformedConfirmedErrorPdu) {
  constexpr std::array<std::uint8_t, 8> missingErrorClass{
      0xa2, 0x06, 0x02, 0x01, 0x07, 0x30, 0x01, 0x00};
  IEC61850::MmsConfirmedErrorPduView decoded;
  EXPECT_FALSE(
      IEC61850::DecodeMmsConfirmedError(missingErrorClass, &decoded).ok());

  constexpr std::array<std::uint8_t, 10> invalidErrorClass{
      0xa2, 0x08, 0x02, 0x01, 0x07, 0x30, 0x03, 0xa0, 0x01, 0x01};
  EXPECT_FALSE(
      IEC61850::DecodeMmsConfirmedError(invalidErrorClass, &decoded).ok());

  constexpr std::array<std::uint8_t, 16> duplicateAdditionalCode{
      0xa2, 0x0e, 0x02, 0x01, 0x07, 0x30, 0x09, 0x87, 0x01, 0x03,
      0x81, 0x01, 0x01, 0x81, 0x01, 0x02};
  EXPECT_FALSE(IEC61850::DecodeMmsConfirmedError(duplicateAdditionalCode,
                                                 &decoded)
                   .ok());
  EXPECT_EQ(decoded.invokeId, 0u);
  EXPECT_EQ(decoded.errorCode, 0);
  EXPECT_TRUE(decoded.additionalDescription.empty());
}

// 验证Identify请求使用标准服务选择，并能解析厂商、型号和版本。
TEST(IEC61850MmsServiceTest, EncodesAndDecodesIdentify) {
  std::array<std::uint8_t, 128> request{};
  std::size_t requestSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsIdentifyRequest(9, request, &requestSize).ok());
  // Identify请求的Confirmed服务值为空，最后一个0x00是长度为0的服务TLV。
  ASSERT_EQ(requestSize, 7u);
  EXPECT_EQ(request[0], 0xa0);
  EXPECT_EQ(request[1], 0x05);
  EXPECT_EQ(request[5], 0xa3);
  EXPECT_EQ(request[6], 0x00);

  constexpr std::array<std::uint8_t, 28> response{
      0xa1, 0x1a, 0x02, 0x01, 0x09, 0xa3, 0x15, 0x30, 0x13,
      0x80, 0x03, 'A', 'C', 'M', 0x81, 0x05, 'M', 'o', 'd', 'e', 'l',
      0x82, 0x05, '1', '.', '2', '.', '3'};
  IEC61850::MmsIdentifyResponse decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsIdentifyResponse(response, 9, &decoded).ok());
  EXPECT_EQ(decoded.vendorName, "ACM");
  EXPECT_EQ(decoded.modelName, "Model");
  EXPECT_EQ(decoded.revision, "1.2.3");
}

// 验证Identify响应invokeID不匹配时不会交付厂商信息。
TEST(IEC61850MmsServiceTest, RejectsIdentifyInvokeIdMismatch) {
  constexpr std::array<std::uint8_t, 18> response{
      0xa1, 0x10, 0x02, 0x01, 0x09, 0xa3, 0x0b, 0x30, 0x09,
      0x80, 0x01, 'A', 0x81, 0x01, 'B', 0x82, 0x01, 'C'};
  IEC61850::MmsIdentifyResponse decoded;
  EXPECT_FALSE(IEC61850::DecodeMmsIdentifyResponse(response, 10, &decoded).ok());
  EXPECT_TRUE(decoded.vendorName.empty());
}

// 验证AR502H使用的FileOpen请求采用GraphicString文件名、初始位置和高标签72。
TEST(IEC61850MmsServiceTest, EncodesFileOpenRequest) {
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsFileOpenRequest(
                  3, "fault.cfg", 0, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsConfirmedPduView pdu;
  ASSERT_TRUE(IEC61850::DecodeMmsConfirmedRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &pdu)
                  .ok());
  EXPECT_EQ(pdu.invokeId, 3u);
  EXPECT_EQ(pdu.serviceTag, 72u);
  ASSERT_FALSE(pdu.serviceValue.empty());
  EXPECT_EQ(pdu.serviceValue.front(), 0xa0);
}

// 验证FileRead/FileClose请求使用原始上下文服务选择，而不是构造标签。
TEST(IEC61850MmsServiceTest, EncodesPrimitiveFileRequests) {
  std::array<std::uint8_t, 64> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsFileReadRequest(4, 17, encoded,
                                                  &encodedSize)
                  .ok());
  IEC61850::MmsConfirmedPduView pdu;
  ASSERT_TRUE(IEC61850::DecodeMmsConfirmedRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &pdu)
                  .ok());
  EXPECT_EQ(pdu.serviceTag, 73u);
  EXPECT_EQ(encoded[5], 0x9f);
  ASSERT_TRUE(IEC61850::EncodeMmsFileCloseRequest(5, -2, encoded,
                                                   &encodedSize)
                  .ok());
  EXPECT_EQ(encoded[5], 0x9f);
  ASSERT_TRUE(IEC61850::DecodeMmsConfirmedRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &pdu)
                  .ok());
  EXPECT_EQ(pdu.serviceTag, 74u);
}

// 验证FileDirectory响应能解析文件名、大小、可选修改时间和分页标志。
TEST(IEC61850MmsServiceTest, DecodesFileDirectoryResponse) {
  constexpr std::array<std::uint8_t, 32> serviceValue{
      0xa0, 0x1b, 0x30, 0x19, 0x30, 0x17, 0xa0, 0x06, 0x19, 0x04, 't', 'e',
      's', 't', 0xa1, 0x0d, 0x80, 0x01, 0x2a, 0x81, 0x08, '2', '0', '2', '4',
      '0', '1', '0', '1', 0x81, 0x01, 0xff};
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsConfirmedResponse(
                  6, 77, serviceValue, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsFileDirectoryResponse decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsFileDirectoryResponse(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  6, &decoded)
                  .ok());
  ASSERT_EQ(decoded.entries.size(), 1u);
  EXPECT_EQ(decoded.entries.front().fileName, "test");
  EXPECT_EQ(decoded.entries.front().fileSize, 42u);
  EXPECT_TRUE(decoded.entries.front().modifiedTimePresent);
  EXPECT_EQ(decoded.entries.front().modifiedTime, "20240101");
  EXPECT_TRUE(decoded.moreFollows);
}

// 验证FileRead响应省略moreFollows时按协议默认值true处理，并拒绝错序服务。
TEST(IEC61850MmsServiceTest, DecodesFileReadResponseDefaults) {
  constexpr std::array<std::uint8_t, 5> serviceValue{0x80, 0x03, 'a', 'b',
                                                     'c'};
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsConfirmedResponse(
                  8, 73, serviceValue,
                  encoded, &encodedSize)
                  .ok());
  IEC61850::MmsFileReadResponse decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsFileReadResponse(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  8, &decoded)
                  .ok());
  EXPECT_EQ(std::string(decoded.data.begin(), decoded.data.end()), "abc");
  EXPECT_TRUE(decoded.moreFollows);
}

// 验证JournalStatus/InitializeJournal请求响应包含对象名、计数和可选游标。
TEST(IEC61850MmsServiceTest, JournalManagementRoundTrip) {
  IEC61850::MmsJournalStatusRequest statusRequest;
  statusRequest.journal.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
  statusRequest.journal.domain = "LD0";
  statusRequest.journal.identifier = "LLN0$JOU1";
  std::array<std::uint8_t, 512> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsJournalStatusRequest(
                  31, statusRequest, encoded, &encodedSize)
                  .ok());
  std::uint32_t invokeId = 0;
  IEC61850::MmsJournalStatusRequest decodedStatus;
  ASSERT_TRUE(IEC61850::DecodeMmsJournalStatusRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &decodedStatus)
                  .ok());
  EXPECT_EQ(invokeId, 31U);
  EXPECT_EQ(decodedStatus.journal, statusRequest.journal);

  IEC61850::MmsJournalStatusResponse statusResponse{42, true};
  ASSERT_TRUE(IEC61850::EncodeMmsJournalStatusResponse(
                  invokeId, statusResponse, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsJournalStatusResponse decodedResponse;
  ASSERT_TRUE(IEC61850::DecodeMmsJournalStatusResponse(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  invokeId, &decodedResponse)
                  .ok());
  EXPECT_EQ(decodedResponse.currentEntries, 42U);
  EXPECT_TRUE(decodedResponse.mmsDeletable);

  IEC61850::MmsInitializeJournalRequest initRequest;
  initRequest.journal = statusRequest.journal;
  initRequest.limitTimeMs = 1700000000123;
  initRequest.limitEntryId = {0x01, 0x02, 0x03};
  ASSERT_TRUE(IEC61850::EncodeMmsInitializeJournalRequest(
                  32, initRequest, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsInitializeJournalRequest decodedInit;
  auto initDecodeStatus = IEC61850::DecodeMmsInitializeJournalRequest(
      std::span<const std::uint8_t>(encoded.data(), encodedSize), &invokeId,
      &decodedInit);
  ASSERT_TRUE(initDecodeStatus.ok()) << initDecodeStatus.error_message();
  EXPECT_EQ(invokeId, 32U);
  EXPECT_EQ(decodedInit.journal, initRequest.journal);
  EXPECT_EQ(decodedInit.limitTimeMs, initRequest.limitTimeMs);
  EXPECT_EQ(decodedInit.limitEntryId, initRequest.limitEntryId);
}

// 验证ReadJournal可结构化输出SOE变量、ReasonCode和分页标志，并拒绝超长EntryID。
TEST(IEC61850MmsServiceTest, JournalReadStructuredRoundTrip) {
  IEC61850::MmsJournalReadRequest request;
  request.journal.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
  request.journal.domain = "LD0";
  request.journal.identifier = "LLN0$JOU1";
  request.startTimeMs = 1700000000000;
  request.endTimeMs = 1700000010000;
  request.numberOfEntries = 10;
  request.variableTags = {"X1", "X2"};
  request.startAfterEntryId = {0x10, 0x11};
  request.startAfterTimeMs = 1700000001000;
  std::array<std::uint8_t, 4096> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsJournalReadRequest(
                  33, request, encoded, &encodedSize)
                  .ok());
  std::uint32_t invokeId = 0;
  IEC61850::MmsJournalReadRequest decodedRequest;
  auto readDecodeStatus = IEC61850::DecodeMmsJournalReadRequest(
      std::span<const std::uint8_t>(encoded.data(), encodedSize), &invokeId,
      &decodedRequest);
  ASSERT_TRUE(readDecodeStatus.ok()) << readDecodeStatus.error_message();
  EXPECT_EQ(invokeId, 33U);
  EXPECT_EQ(decodedRequest.journal, request.journal);
  EXPECT_EQ(decodedRequest.startTimeMs, request.startTimeMs);
  EXPECT_EQ(decodedRequest.endTimeMs, request.endTimeMs);
  EXPECT_EQ(decodedRequest.numberOfEntries, request.numberOfEntries);
  EXPECT_EQ(decodedRequest.variableTags, request.variableTags);
  EXPECT_EQ(decodedRequest.startAfterEntryId, request.startAfterEntryId);

  IEC61850::MmsJournalReadResponse response;
  response.moreFollows = true;
  auto& entry = response.entries.emplace_back();
  entry.entryId = {0x10, 0x11};
  entry.occurrenceTimeMs = 1700000001000;
  entry.kind = IEC61850::MmsJournalEntryKind::SOE;
  entry.eventCondition = "XSWI1.Pos";
  entry.currentState = 2;
  auto& variable = entry.variables.emplace_back();
  variable.tag = "X1";
  variable.reasonCode = 3;
  ASSERT_TRUE(IEC61850::EncodeMmsDataBoolean(true, &variable.encodedData).ok());
  ASSERT_TRUE(IEC61850::EncodeMmsJournalReadResponse(
                  33, response, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsJournalReadResponse decodedResponse;
  ASSERT_TRUE(IEC61850::DecodeMmsJournalReadResponse(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  33, &decodedResponse)
                  .ok());
  ASSERT_EQ(decodedResponse.entries.size(), 1U);
  EXPECT_EQ(decodedResponse.entries.front().entryId, entry.entryId);
  EXPECT_EQ(decodedResponse.entries.front().occurrenceTimeMs,
            entry.occurrenceTimeMs);
  EXPECT_EQ(decodedResponse.entries.front().kind,
            IEC61850::MmsJournalEntryKind::SOE);
  ASSERT_EQ(decodedResponse.entries.front().variables.size(), 1U);
  EXPECT_EQ(decodedResponse.entries.front().variables.front().reasonCode, 3U);
}

// 验证动态DataSet/NVL定义、删除请求的服务选择和成员顺序。
TEST(IEC61850MmsServiceTest, DynamicDataSetRoundTrip) {
  IEC61850::MmsNamedVariableListDefinition definition;
  definition.listName.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
  definition.listName.domain = "LD0";
  definition.listName.identifier = "LLN0$DS1";
  IEC61850::MmsObjectName member = definition.listName;
  member.identifier = "LLN0$ST$X1";
  definition.variables.push_back(member);
  std::array<std::uint8_t, 1024> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsDefineNamedVariableListRequest(
                  34, definition, encoded, &encodedSize)
                  .ok());
  std::uint32_t invokeId = 0;
  IEC61850::MmsNamedVariableListDefinition decoded;
  auto defineDecodeStatus = IEC61850::DecodeMmsDefineNamedVariableListRequest(
      std::span<const std::uint8_t>(encoded.data(), encodedSize), &invokeId,
      &decoded);
  ASSERT_TRUE(defineDecodeStatus.ok()) << defineDecodeStatus.error_message();
  EXPECT_EQ(invokeId, 34U);
  EXPECT_EQ(decoded.listName, definition.listName);
  ASSERT_EQ(decoded.variables.size(), 1U);
  EXPECT_EQ(decoded.variables.front(), member);

  ASSERT_TRUE(IEC61850::EncodeMmsDeleteNamedVariableListRequest(
                  35, definition.listName, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsObjectName deleted;
  ASSERT_TRUE(IEC61850::DecodeMmsDeleteNamedVariableListRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &deleted)
                  .ok());
  EXPECT_EQ(invokeId, 35U);
  EXPECT_EQ(deleted, definition.listName);
}

// 验证真正FileUpload三段服务以及删除/改名服务使用独立服务选择和句柄。
TEST(IEC61850MmsServiceTest, FileMutationAndUploadRoundTrip) {
  std::array<std::uint8_t, 2048> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsInitiateDownloadRequest(
                  36, {"VMD", "/COMTRADE/a.cfg", 12}, encoded, &encodedSize)
                  .ok());
  std::uint32_t invokeId = 0;
  IEC61850::MmsInitiateDownloadRequest initiate;
  ASSERT_TRUE(IEC61850::DecodeMmsInitiateDownloadRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &initiate)
                  .ok());
  EXPECT_EQ(invokeId, 36U);
  EXPECT_EQ(initiate.fileName, "/COMTRADE/a.cfg");
  EXPECT_EQ(initiate.fileSize, 12U);

  IEC61850::MmsDownloadSegmentRequest segment;
  segment.frsmId = 7;
  segment.data = {1, 2, 3};
  ASSERT_TRUE(IEC61850::EncodeMmsDownloadSegmentRequest(
                  37, segment, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsDownloadSegmentRequest decodedSegment;
  ASSERT_TRUE(IEC61850::DecodeMmsDownloadSegmentRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &decodedSegment)
                  .ok());
  EXPECT_EQ(decodedSegment.frsmId, 7);
  EXPECT_EQ(decodedSegment.data, segment.data);

  IEC61850::MmsTerminateDownloadRequest terminate{7};
  ASSERT_TRUE(IEC61850::EncodeMmsTerminateDownloadRequest(
                  38, terminate, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsTerminateDownloadRequest decodedTerminate;
  ASSERT_TRUE(IEC61850::DecodeMmsTerminateDownloadRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &decodedTerminate)
                  .ok());
  EXPECT_EQ(decodedTerminate.frsmId, 7);

  ASSERT_TRUE(IEC61850::EncodeMmsFileDeleteRequest(
                  39, "/COMTRADE/a.cfg", encoded, &encodedSize)
                  .ok());
  IEC61850::MmsConfirmedPduView pdu;
  ASSERT_TRUE(IEC61850::DecodeMmsConfirmedRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &pdu)
                  .ok());
  EXPECT_EQ(pdu.serviceTag, 76U);
  ASSERT_TRUE(IEC61850::EncodeMmsFileRenameRequest(
                  40, "a.cfg", "b.cfg", encoded, &encodedSize)
                  .ok());
  ASSERT_TRUE(IEC61850::DecodeMmsConfirmedRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &pdu)
                  .ok());
  EXPECT_EQ(pdu.serviceTag, 75U);
}

// 验证GetVariableAccessAttributes的Domain对象名按domainId、itemId顺序编码。
TEST(IEC61850MmsServiceTest, EncodesDomainVariableAccessAttributesRequest) {
  IEC61850::MmsObjectName name;
  name.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
  name.domain = "LD0";
  name.identifier = "LLN0$Mod$stVal";
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsGetVariableAccessAttributesRequest(
                  3, name, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsConfirmedPduView pdu;
  ASSERT_TRUE(IEC61850::DecodeMmsConfirmedRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &pdu)
                  .ok());
  EXPECT_EQ(pdu.invokeId, 3u);
  EXPECT_EQ(pdu.serviceTag, 6u);
  ASSERT_FALSE(pdu.serviceValue.empty());
  EXPECT_EQ(pdu.serviceValue.front(), 0xa0);
}

// 验证GetVariableAccessAttributes响应能解析mmsDeletable和基础BOOLEAN类型。
TEST(IEC61850MmsServiceTest, DecodesVariableAccessAttributesBooleanResponse) {
  constexpr std::array<std::uint8_t, 14> response{
      0xa1, 0x0c, 0x02, 0x01, 0x03, 0xa6, 0x07, 0x80, 0x01, 0x00,
      0xa2, 0x02, 0x83, 0x00};
  IEC61850::MmsGetVariableAccessAttributesResponse decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsGetVariableAccessAttributesResponse(
                  response, 3, &decoded)
                  .ok());
  EXPECT_FALSE(decoded.mmsDeletable);
  EXPECT_EQ(decoded.typeSpecification.kind,
            IEC61850::MmsTypeSpecificationKind::BOOLEAN);
}

// 验证TypeSpecification中的结构成员、数组和无符号位宽能够递归解析。
TEST(IEC61850MmsServiceTest, DecodesVariableAccessAttributesNestedTypes) {
  constexpr std::array<std::uint8_t, 43> response{
      0xa1, 0x29, 0x02, 0x01, 0x04, 0xa6, 0x24, 0x80, 0x01, 0xff,
      0xa2, 0x1f, 0xa2, 0x1d, 0xa1, 0x1b, 0x30, 0x07, 0x80, 0x01,
      'A', 0xa1, 0x02, 0x83, 0x00, 0x30, 0x10, 0x80, 0x01, 'B',
      0xa1, 0x0b, 0x80, 0x01, 0xff, 0x81, 0x01, 0x02, 0xa2, 0x03,
      0x86, 0x01, 0x10};
  IEC61850::MmsGetVariableAccessAttributesResponse decoded;
  const auto status = IEC61850::DecodeMmsGetVariableAccessAttributesResponse(
      response, 4, &decoded);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(decoded.mmsDeletable);
  ASSERT_EQ(decoded.typeSpecification.kind,
            IEC61850::MmsTypeSpecificationKind::STRUCTURE);
  ASSERT_EQ(decoded.typeSpecification.components.size(), 2u);
  EXPECT_EQ(decoded.typeSpecification.components[0].name, "A");
  ASSERT_TRUE(decoded.typeSpecification.components[0].type);
  EXPECT_EQ(decoded.typeSpecification.components[0].type->kind,
            IEC61850::MmsTypeSpecificationKind::BOOLEAN);
  EXPECT_EQ(decoded.typeSpecification.components[1].name, "B");
  ASSERT_TRUE(decoded.typeSpecification.components[1].type);
  EXPECT_EQ(decoded.typeSpecification.components[1].type->kind,
            IEC61850::MmsTypeSpecificationKind::ARRAY);
  EXPECT_EQ(decoded.typeSpecification.components[1].type->elementCount, 2u);
  ASSERT_TRUE(decoded.typeSpecification.components[1].type->elementType);
  EXPECT_EQ(decoded.typeSpecification.components[1].type->elementType->kind,
            IEC61850::MmsTypeSpecificationKind::UNSIGNED);
  EXPECT_EQ(decoded.typeSpecification.components[1].type->elementType->width,
            16u);
}

// 验证TypeSpecification未知选择和invokeID不匹配时拒绝整个响应。
TEST(IEC61850MmsServiceTest, RejectsInvalidVariableAccessAttributesResponse) {
  constexpr std::array<std::uint8_t, 14> response{
      0xa1, 0x0c, 0x02, 0x01, 0x03, 0xa6, 0x07, 0x80, 0x01, 0x00,
      0xa2, 0x02, 0x88, 0x00};
  IEC61850::MmsGetVariableAccessAttributesResponse decoded;
  EXPECT_FALSE(IEC61850::DecodeMmsGetVariableAccessAttributesResponse(
                   response, 3, &decoded)
                   .ok());
  EXPECT_FALSE(IEC61850::DecodeMmsGetVariableAccessAttributesResponse(
                   response, 4, &decoded)
                   .ok());
}

// 验证GetNamedVariableListAttributes响应能保留DataSet成员顺序和Domain对象名。
TEST(IEC61850MmsServiceTest, DecodesNamedVariableListAttributesResponse) {
  constexpr std::array<std::uint8_t, 26> response{
      0xa1, 0x18, 0x02, 0x01, 0x03, 0xac, 0x13, 0x80, 0x01, 0x00,
      0xa1, 0x0e, 0x30, 0x0c, 0xa0, 0x0a, 0xa1, 0x08, 0x1a, 0x03,
      'L',  'D',  '0',  0x1a, 0x01, 'A'};
  IEC61850::MmsGetNamedVariableListAttributesResponse decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsGetNamedVariableListAttributesResponse(
                  response, 3, &decoded)
                  .ok());
  EXPECT_FALSE(decoded.mmsDeletable);
  ASSERT_EQ(decoded.variables.size(), 1u);
  EXPECT_EQ(decoded.variables.front().type,
            IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC);
  EXPECT_EQ(decoded.variables.front().domain, "LD0");
  EXPECT_EQ(decoded.variables.front().identifier, "A");
}

// 验证GetNamedVariableListAttributes请求使用Domain对象名和服务选择12。
TEST(IEC61850MmsServiceTest, EncodesNamedVariableListAttributesRequest) {
  IEC61850::MmsObjectName name;
  name.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
  name.domain = "LD0";
  name.identifier = "LLN0$measurements";
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsGetNamedVariableListAttributesRequest(
                  9, name, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsConfirmedPduView pdu;
  ASSERT_TRUE(IEC61850::DecodeMmsConfirmedRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &pdu)
                  .ok());
  EXPECT_EQ(pdu.invokeId, 9u);
  EXPECT_EQ(pdu.serviceTag, 12u);
  ASSERT_FALSE(pdu.serviceValue.empty());
  EXPECT_EQ(pdu.serviceValue.front(), 0xa1);
}

// 验证GetNamedVariableListAttributes响应的invokeID或成员结构错误会被拒绝。
TEST(IEC61850MmsServiceTest, RejectsInvalidNamedVariableListAttributesResponse) {
  constexpr std::array<std::uint8_t, 26> response{
      0xa1, 0x18, 0x02, 0x01, 0x03, 0xac, 0x13, 0x80, 0x01, 0x00,
      0xa1, 0x0e, 0x30, 0x0c, 0xa0, 0x0a, 0xa1, 0x08, 0x1a, 0x03,
      'L',  'D',  '0',  0x1a, 0x01, 'A'};
  IEC61850::MmsGetNamedVariableListAttributesResponse decoded;
  EXPECT_FALSE(IEC61850::DecodeMmsGetNamedVariableListAttributesResponse(
                   response, 4, &decoded)
                   .ok());

  auto malformed = response;
  malformed[14] = 0xa1;
  EXPECT_FALSE(IEC61850::DecodeMmsGetNamedVariableListAttributesResponse(
                   malformed, 3, &decoded)
                   .ok());
}

// 验证超长Identifier不会绕过服务层的有界编码限制。
TEST(IEC61850MmsServiceTest, RejectsOversizedIdentifier) {
  IEC61850::MmsGetNameListRequest request;
  request.continueAfter = std::string(1025, 'x');
  std::array<std::uint8_t, 2048> encoded{};
  std::size_t encodedSize = 99;
  EXPECT_FALSE(IEC61850::EncodeMmsGetNameListRequest(
                   1, request, encoded, &encodedSize)
                   .ok());
  EXPECT_EQ(encodedSize, 0u);
}

// 验证Read请求的listOfVariable对象名和invokeID能够完整往返。
TEST(IEC61850MmsServiceTest, EncodesAndDecodesReadRequest) {
  IEC61850::MmsReadRequest request;
  request.variables.push_back(
      {.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC,
       .domain = "LD0",
       .identifier = "LLN0$ST$Mod$stVal"});
  request.variables.push_back(
      {.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC,
       .domain = "LD0",
       .identifier = "MMXU1$TotW$mag$f"});

  std::array<std::uint8_t, 512> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsReadRequest(13, request, encoded,
                                             &encodedSize)
                  .ok());

  IEC61850::MmsReadRequest decoded;
  std::uint32_t invokeId = 0;
  ASSERT_TRUE(IEC61850::DecodeMmsReadRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &decoded)
                  .ok());
  EXPECT_EQ(invokeId, 13u);
  EXPECT_FALSE(decoded.specificationWithResult);
  ASSERT_EQ(decoded.variables.size(), request.variables.size());
  EXPECT_EQ(decoded.variables[0].domain, "LD0");
  EXPECT_EQ(decoded.variables[0].identifier, "LLN0$ST$Mod$stVal");
  EXPECT_EQ(decoded.variables[1].identifier, "MMXU1$TotW$mag$f");
}

// 验证Read请求需要SpecificationWithResult时按标准字段顺序编码可选[0]字段。
TEST(IEC61850MmsServiceTest, EncodesReadSpecificationWithResult) {
  IEC61850::MmsReadRequest request;
  request.specificationWithResult = true;
  request.variables.push_back(
      {.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC,
       .domain = "LD0",
       .identifier = "LLN0$ST$Mod$stVal"});
  std::array<std::uint8_t, 256> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsReadRequest(14, request, encoded,
                                             &encodedSize)
                  .ok());
  IEC61850::MmsConfirmedPduView pdu;
  ASSERT_TRUE(IEC61850::DecodeMmsConfirmedRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &pdu)
                  .ok());
  ASSERT_FALSE(pdu.serviceValue.empty());
  EXPECT_EQ(pdu.serviceValue.front(), 0x80);

  std::uint32_t invokeId = 0;
  IEC61850::MmsReadRequest decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsReadRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &decoded)
                  .ok());
  EXPECT_EQ(invokeId, 14u);
  EXPECT_TRUE(decoded.specificationWithResult);
}

// 验证Read响应同时包含成功Data和失败DataAccessError时能够保留各自的选择编码。
TEST(IEC61850MmsServiceTest, EncodesAndDecodesReadResponseItems) {
  IEC61850::MmsReadResponse response;
  IEC61850::MmsReadResponseItem success;
  success.success = true;
  success.encodedData = {0x83, 0x01, 0xff};
  response.items.emplace_back(std::move(success));
  IEC61850::MmsReadResponseItem failure;
  failure.success = false;
  failure.failure = {0x80, 0x01, 0x02};
  response.items.emplace_back(std::move(failure));

  std::array<std::uint8_t, 256> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsReadResponse(21, response, encoded,
                                              &encodedSize)
                  .ok());
  IEC61850::MmsReadResponse decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsReadResponse(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  21, &decoded)
                  .ok());
  ASSERT_EQ(decoded.items.size(), 2u);
  EXPECT_TRUE(decoded.items[0].success);
  EXPECT_EQ(decoded.items[0].encodedData,
            (std::vector<std::uint8_t>{0x83, 0x01, 0xff}));
  EXPECT_FALSE(decoded.items[1].success);
  EXPECT_EQ(decoded.items[1].failure,
            (std::vector<std::uint8_t>{0x80, 0x01, 0x02}));
}

// 验证Read响应invokeID不匹配或缺少Data选择时会被拒绝。
TEST(IEC61850MmsServiceTest, RejectsMalformedReadResponse) {
  IEC61850::MmsReadResponse response;
  IEC61850::MmsReadResponseItem item;
  item.success = true;
  item.encodedData = {0x83, 0x01, 0xff};
  response.items.emplace_back(std::move(item));
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsReadResponse(1, response, encoded,
                                              &encodedSize)
                  .ok());
  IEC61850::MmsReadResponse decoded;
  EXPECT_FALSE(IEC61850::DecodeMmsReadResponse(
                   std::span<const std::uint8_t>(encoded.data(), encodedSize),
                   2, &decoded)
                   .ok());

  auto malformed = encoded;
  malformed[0] = 0xa0;
  EXPECT_FALSE(IEC61850::DecodeMmsReadResponse(
                   std::span<const std::uint8_t>(malformed.data(), encodedSize),
                   1, &decoded)
                   .ok());

  IEC61850::MmsReadResponse failureResponse;
  IEC61850::MmsReadResponseItem failure;
  failure.success = false;
  failure.failure = {0x80, 0x01, 0xff};
  failureResponse.items.emplace_back(std::move(failure));
  EXPECT_FALSE(IEC61850::EncodeMmsReadResponse(
                   1, failureResponse, encoded, &encodedSize)
                   .ok());

  IEC61850::MmsReadResponse unknownResponse;
  IEC61850::MmsReadResponseItem unknown;
  unknown.success = true;
  unknown.encodedData = {0x95, 0x00};
  unknownResponse.items.emplace_back(std::move(unknown));
  EXPECT_FALSE(IEC61850::EncodeMmsReadResponse(
                   1, unknownResponse, encoded, &encodedSize)
                   .ok());
}

// 验证Read响应编码器拒绝包含多个BER TLV的单个结果项。
TEST(IEC61850MmsServiceTest, RejectsReadResponseItemWithTrailingTlv) {
  IEC61850::MmsReadResponse response;
  auto& item = response.items.emplace_back();
  item.success = true;
  item.encodedData = {0x83, 0x01, 0xff, 0x83, 0x01, 0x00};
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;

  EXPECT_FALSE(IEC61850::EncodeMmsReadResponse(
                   1, response, encoded, &encodedSize)
                   .ok());
}

// 验证NameList、目录属性、Read和Write解码在后续字段损坏时不会泄漏前面已解析的部分结果。
TEST(IEC61850MmsServiceTest, ClearsPartialMmsDecodeOutputsOnFailure) {
  IEC61850::MmsGetNameListRequest nameListRequest;
  nameListRequest.objectClass = IEC61850::MmsObjectClass::NAMED_VARIABLE;
  nameListRequest.scope.type = IEC61850::MmsObjectScopeType::VMD_SPECIFIC;
  nameListRequest.continueAfter = "LLN0";
  std::array<std::uint8_t, 128> nameListRequestEncoded{};
  std::size_t nameListRequestSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsGetNameListRequest(
                  8, nameListRequest, nameListRequestEncoded,
                  &nameListRequestSize)
                  .ok());
  std::vector<std::uint8_t> malformedNameListRequest(
      nameListRequestEncoded.begin(),
      nameListRequestEncoded.begin() + nameListRequestSize);
  const auto continuationTag = std::find(
      malformedNameListRequest.begin(), malformedNameListRequest.end(),
      static_cast<std::uint8_t>(0x82));
  ASSERT_NE(continuationTag, malformedNameListRequest.end());
  ASSERT_NE(continuationTag + 1, malformedNameListRequest.end());
  *(continuationTag + 1) = 0x82;
  std::uint32_t nameListInvokeId = 0;
  IEC61850::MmsGetNameListRequest decodedNameListRequest;
  EXPECT_FALSE(IEC61850::DecodeMmsGetNameListRequest(
                   malformedNameListRequest, &nameListInvokeId,
                   &decodedNameListRequest)
                   .ok());
  EXPECT_EQ(nameListInvokeId, 0u);
  EXPECT_EQ(decodedNameListRequest.objectClass,
            IEC61850::MmsObjectClass::DOMAIN);
  EXPECT_EQ(decodedNameListRequest.scope.type,
            IEC61850::MmsObjectScopeType::VMD_SPECIFIC);
  EXPECT_TRUE(decodedNameListRequest.scope.domain.empty());
  EXPECT_FALSE(decodedNameListRequest.continueAfter.has_value());

  constexpr std::array<std::uint8_t, 22> nameListResponse{
      0xa1, 0x14, 0x02, 0x01, 0x07, 0xa1, 0x0f, 0xa0, 0x0a,
      0x1a, 0x03, 0x4c, 0x44, 0x30, 0x1a, 0x03, 0x4c, 0x44,
      0x31, 0x81, 0x01, 0x00};
  auto malformedNameList = nameListResponse;
  malformedNameList[20] = 0x82;
  IEC61850::MmsGetNameListResponse decodedNameList;
  EXPECT_FALSE(IEC61850::DecodeMmsGetNameListResponse(
                   malformedNameList, 7, &decodedNameList)
                   .ok());
  EXPECT_TRUE(decodedNameList.identifiers.empty());

  IEC61850::MmsReadRequest readRequest;
  readRequest.variables = {
      {.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC,
       .domain = "LD0",
       .identifier = "LLN0$A"},
      {.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC,
       .domain = "LD0",
       .identifier = "LLN0$B"}};
  std::array<std::uint8_t, 256> readRequestEncoded{};
  std::size_t readRequestSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsReadRequest(
                  9, readRequest, readRequestEncoded, &readRequestSize)
                  .ok());
  std::vector<std::uint8_t> malformedReadRequest(
      readRequestEncoded.begin(), readRequestEncoded.begin() + readRequestSize);
  const auto secondVariableChoice = std::find(
      malformedReadRequest.rbegin(), malformedReadRequest.rend(),
      static_cast<std::uint8_t>(0xa0));
  ASSERT_NE(secondVariableChoice, malformedReadRequest.rend());
  *std::prev(secondVariableChoice.base()) = 0xa3;
  std::uint32_t readInvokeId = 0;
  IEC61850::MmsReadRequest decodedReadRequest;
  EXPECT_FALSE(IEC61850::DecodeMmsReadRequest(
                   malformedReadRequest, &readInvokeId, &decodedReadRequest)
                   .ok());
  EXPECT_EQ(readInvokeId, 0u);
  EXPECT_TRUE(decodedReadRequest.variables.empty());

  IEC61850::MmsReadResponse readResponse;
  readResponse.items.resize(2);
  for (auto& item : readResponse.items) {
    item.success = true;
    ASSERT_TRUE(IEC61850::EncodeMmsDataBoolean(true, &item.encodedData).ok());
  }
  std::array<std::uint8_t, 256> readResponseEncoded{};
  std::size_t readResponseSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsReadResponse(
                  10, readResponse, readResponseEncoded, &readResponseSize)
                  .ok());
  std::vector<std::uint8_t> malformedReadResponse(
      readResponseEncoded.begin(), readResponseEncoded.begin() + readResponseSize);
  const auto secondData = std::find(
      malformedReadResponse.rbegin(), malformedReadResponse.rend(),
      static_cast<std::uint8_t>(0x83));
  ASSERT_NE(secondData, malformedReadResponse.rend());
  *std::prev(secondData.base()) = 0x95;
  IEC61850::MmsReadResponse decodedReadResponse;
  EXPECT_FALSE(IEC61850::DecodeMmsReadResponse(malformedReadResponse, 10,
                                                &decodedReadResponse)
                   .ok());
  EXPECT_TRUE(decodedReadResponse.items.empty());

  std::vector<std::uint8_t> attributesService{
      0x80, 0x01, 0x00, 0xa1, 0x00, 0xa2, 0x02, 0x83, 0x00};
  std::array<std::uint8_t, 128> attributesEncoded{};
  std::size_t attributesSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsConfirmedResponse(
                  11, 6, attributesService, attributesEncoded,
                  &attributesSize)
                  .ok());
  auto malformedAttributes = std::vector<std::uint8_t>(
      attributesEncoded.begin(), attributesEncoded.begin() + attributesSize);
  const auto booleanType = std::find(malformedAttributes.rbegin(),
                                     malformedAttributes.rend(),
                                     static_cast<std::uint8_t>(0x83));
  ASSERT_NE(booleanType, malformedAttributes.rend());
  *std::prev(booleanType.base()) = 0x88;
  IEC61850::MmsGetVariableAccessAttributesResponse decodedAttributes;
  decodedAttributes.addressPresent = true;
  EXPECT_FALSE(IEC61850::DecodeMmsGetVariableAccessAttributesResponse(
                   malformedAttributes, 11, &decodedAttributes)
                   .ok());
  EXPECT_FALSE(decodedAttributes.addressPresent);

  constexpr std::array<std::uint8_t, 29> namedListResponse{
      0xa1, 0x1b, 0x02, 0x01, 0x03, 0xac, 0x16, 0x80, 0x01, 0x00,
      0xa1, 0x11, 0x30, 0x0c, 0xa0, 0x0a, 0xa1, 0x08, 0x1a, 0x03,
      'L',  'D',  '0',  0x1a, 0x01, 'A', 0x30, 0x01, 0x00};
  IEC61850::MmsGetNamedVariableListAttributesResponse decodedNamedList;
  EXPECT_FALSE(IEC61850::DecodeMmsGetNamedVariableListAttributesResponse(
                   namedListResponse, 3, &decodedNamedList)
                   .ok());
  EXPECT_TRUE(decodedNamedList.variables.empty());

  IEC61850::MmsWriteResponse writeResponse;
  writeResponse.items = {{true, 0}, {true, 0}};
  std::array<std::uint8_t, 128> writeResponseEncoded{};
  std::size_t writeResponseSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsWriteResponse(
                  12, writeResponse, writeResponseEncoded, &writeResponseSize)
                  .ok());
  auto malformedWriteResponse = std::vector<std::uint8_t>(
      writeResponseEncoded.begin(), writeResponseEncoded.begin() +
                                        writeResponseSize);
  const auto secondSuccess = std::find(malformedWriteResponse.rbegin(),
                                       malformedWriteResponse.rend(),
                                       static_cast<std::uint8_t>(0x81));
  ASSERT_NE(secondSuccess, malformedWriteResponse.rend());
  *std::prev(secondSuccess.base()) = 0x95;
  IEC61850::MmsWriteResponse decodedWriteResponse;
  EXPECT_FALSE(IEC61850::DecodeMmsWriteResponse(
                   malformedWriteResponse, 12, &decodedWriteResponse)
                   .ok());
  EXPECT_TRUE(decodedWriteResponse.items.empty());
}

// 验证Write请求的多变量对象名、BOOLEAN/Unsigned/VisibleString/BIT STRING Data能够往返。
TEST(IEC61850MmsServiceTest, EncodesAndDecodesWriteRequest) {
  IEC61850::MmsWriteRequest request;
  std::vector<std::uint8_t> booleanData;
  std::vector<std::uint8_t> unsignedData;
  std::vector<std::uint8_t> visibleData;
  std::vector<std::uint8_t> bitStringData;
  ASSERT_TRUE(IEC61850::EncodeMmsDataBoolean(true, &booleanData).ok());
  ASSERT_TRUE(IEC61850::EncodeMmsDataUnsigned(0x1234, &unsignedData).ok());
  ASSERT_TRUE(IEC61850::EncodeMmsDataVisibleString("Rpt-1", &visibleData).ok());
  const std::array<std::uint8_t, 1> bitStringPayload{0xa8};
  ASSERT_TRUE(IEC61850::EncodeMmsDataBitString(
                  2, bitStringPayload, &bitStringData)
                  .ok());
  request.items = {
      {{IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC, "LD0",
        "LLN0$R"},
       booleanData},
      {{IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC, "LD0",
        "LLN0$N"},
       unsignedData},
      {{IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC, "LD0",
        "LLN0$S"},
       visibleData},
      {{IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC, "LD0",
        "LLN0$B"},
       bitStringData},
  };

  std::array<std::uint8_t, 1024> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsWriteRequest(7, request, encoded,
                                               &encodedSize)
                  .ok());

  IEC61850::MmsWriteRequest decoded;
  std::uint32_t invokeId = 0;
  ASSERT_TRUE(IEC61850::DecodeMmsWriteRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &decoded)
                  .ok());
  EXPECT_EQ(invokeId, 7u);
  ASSERT_EQ(decoded.items.size(), request.items.size());
  for (std::size_t index = 0; index < request.items.size(); ++index) {
    EXPECT_EQ(decoded.items[index].variable.domain, "LD0");
    EXPECT_EQ(decoded.items[index].variable.identifier,
              request.items[index].variable.identifier);
    EXPECT_EQ(decoded.items[index].encodedData,
              request.items[index].encodedData);
  }
}

// 验证MMS Signed INTEGER使用最短补码编码，并保留正数符号补零。
TEST(IEC61850MmsServiceTest, EncodesSignedDataWithCanonicalInteger) {
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(IEC61850::EncodeMmsDataSigned(-128, &encoded).ok());
  EXPECT_EQ(encoded, (std::vector<std::uint8_t>{0x85, 0x01, 0x80}));

  ASSERT_TRUE(IEC61850::EncodeMmsDataSigned(128, &encoded).ok());
  EXPECT_EQ(encoded,
            (std::vector<std::uint8_t>{0x85, 0x02, 0x00, 0x80}));

  for (const auto value : {std::int64_t{0}, std::int64_t{-1},
                           std::int64_t{127}, std::int64_t{-129},
                           std::numeric_limits<std::int64_t>::min(),
                           std::numeric_limits<std::int64_t>::max()}) {
    ASSERT_TRUE(IEC61850::EncodeMmsDataSigned(value, &encoded).ok());
    std::size_t offset = 0;
    IEC61850::BerTlvView tlv;
    ASSERT_TRUE(IEC61850::ReadBerTlv(encoded, &offset, &tlv).ok());
    ASSERT_EQ(offset, encoded.size());
    std::int64_t decoded = 0;
    ASSERT_TRUE(IEC61850::ReadBerSigned(tlv.value, &decoded).ok());
    EXPECT_EQ(decoded, value);
  }
}

// 验证MMS FLOATING-POINT按format-width和大端IEEE-754值编码。
TEST(IEC61850MmsServiceTest, EncodesFloatingPointDataWithFormatWidth) {
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(IEC61850::EncodeMmsDataFloatingPoint(1.5, 0x08, &encoded)
                  .ok());
  EXPECT_EQ(encoded, (std::vector<std::uint8_t>{0x87, 0x05, 0x08, 0x3f,
                                                0xc0, 0x00, 0x00}));

  ASSERT_TRUE(IEC61850::EncodeMmsDataFloatingPoint(-2.25, 0x0b, &encoded)
                  .ok());
  EXPECT_EQ(encoded,
            (std::vector<std::uint8_t>{0x87, 0x09, 0x0b, 0xc0, 0x02, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00}));
}

// 验证MMS BinaryTime和UTC time分别按标准长度、时间基准和时钟质量编码。
TEST(IEC61850MmsServiceTest, EncodesBinaryAndUtcTimeData) {
  constexpr std::int64_t kMillisecondsPerDay = 86400000;
  constexpr std::int64_t kEpoch1984Ms = 5113 * kMillisecondsPerDay;
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(IEC61850::EncodeMmsDataBinaryTime(
                  kEpoch1984Ms + kMillisecondsPerDay + 123, &encoded)
                  .ok());
  EXPECT_EQ(encoded,
            (std::vector<std::uint8_t>{0x8c, 0x06, 0x00, 0x01, 0x00, 0x00,
                                       0x00, 0x7b}));

  ASSERT_TRUE(IEC61850::EncodeMmsDataBinaryTime(
                  kEpoch1984Ms + 65535 * kMillisecondsPerDay +
                      kMillisecondsPerDay - 1,
                  &encoded)
                  .ok());
  EXPECT_EQ(encoded,
            (std::vector<std::uint8_t>{0x8c, 0x06, 0xff, 0xff, 0x05, 0x26,
                                       0x5b, 0xff}));

  ASSERT_TRUE(IEC61850::EncodeMmsDataUtcTime(1123, false, &encoded).ok());
  EXPECT_EQ(encoded,
            (std::vector<std::uint8_t>{0x91, 0x08, 0x00, 0x00, 0x00, 0x01,
                                       0x1f, 0x7c, 0xed, 0x80}));

  ASSERT_TRUE(IEC61850::EncodeMmsDataUtcTime(
                  static_cast<std::int64_t>(
                      std::numeric_limits<std::uint32_t>::max()) *
                      1000,
                  true, &encoded)
                  .ok());
  EXPECT_EQ(encoded,
            (std::vector<std::uint8_t>{0x91, 0x08, 0xff, 0xff, 0xff, 0xff,
                                       0x00, 0x00, 0x00, 0x00}));
}

// 验证新Data编码辅助拒绝空输出、非法浮点宽度、非有限值和越界时间。
TEST(IEC61850MmsServiceTest, RejectsInvalidExtendedDataEncodingArguments) {
  std::vector<std::uint8_t> encoded{0xff};
  EXPECT_FALSE(IEC61850::EncodeMmsDataSigned(1, nullptr).ok());
  EXPECT_FALSE(
      IEC61850::EncodeMmsDataFloatingPoint(1.0, 0x04, &encoded).ok());
  EXPECT_TRUE(encoded.empty());
  EXPECT_FALSE(IEC61850::EncodeMmsDataFloatingPoint(
                   std::numeric_limits<double>::quiet_NaN(), 0x08, &encoded)
                   .ok());
  EXPECT_FALSE(IEC61850::EncodeMmsDataFloatingPoint(
                   std::numeric_limits<double>::infinity(), 0x08, &encoded)
                   .ok());
  EXPECT_FALSE(IEC61850::EncodeMmsDataFloatingPoint(
                   std::numeric_limits<double>::max(), 0x08, &encoded)
                   .ok());
  EXPECT_FALSE(IEC61850::EncodeMmsDataBinaryTime(0, &encoded).ok());
  constexpr std::int64_t kEpoch1984Ms =
      static_cast<std::int64_t>(5113) * 86400000;
  EXPECT_FALSE(IEC61850::EncodeMmsDataBinaryTime(
                   kEpoch1984Ms + static_cast<std::int64_t>(65536) * 86400000,
                   &encoded)
                   .ok());
  EXPECT_FALSE(IEC61850::EncodeMmsDataUtcTime(-1, true, &encoded).ok());
  EXPECT_FALSE(IEC61850::EncodeMmsDataUtcTime(
                   (static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) +
                    1) * 1000,
                   true, &encoded)
                   .ok());
}

// 验证Write响应成功项和DataAccessError失败项能够往返，并核对invokeID。
TEST(IEC61850MmsServiceTest, EncodesAndDecodesWriteResponse) {
  IEC61850::MmsWriteResponse response;
  response.items = {{true, 0}, {false, 3}, {true, 0}};
  std::array<std::uint8_t, 128> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsWriteResponse(19, response, encoded,
                                                &encodedSize)
                  .ok());

  IEC61850::MmsWriteResponse decoded;
  ASSERT_TRUE(IEC61850::DecodeMmsWriteResponse(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  19, &decoded)
                  .ok());
  ASSERT_EQ(decoded.items.size(), 3u);
  EXPECT_TRUE(decoded.items[0].success);
  EXPECT_FALSE(decoded.items[1].success);
  EXPECT_EQ(decoded.items[1].failureCode, 3);
  EXPECT_TRUE(decoded.items[2].success);
  EXPECT_FALSE(IEC61850::DecodeMmsWriteResponse(
                   std::span<const std::uint8_t>(encoded.data(), encodedSize),
                   20, &decoded)
                   .ok());
}

// 验证Write服务拒绝非法Data选择、变量与Data数量不一致以及越界错误码。
TEST(IEC61850MmsServiceTest, RejectsMalformedWriteMessages) {
  IEC61850::MmsWriteRequest request;
  std::vector<std::uint8_t> data;
  ASSERT_TRUE(IEC61850::EncodeMmsDataBoolean(true, &data).ok());
  request.items.push_back({
      {IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC, "LD0", "LLN0$R"},
      data});
  std::array<std::uint8_t, 256> encoded{};
  std::size_t encodedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsWriteRequest(1, request, encoded,
                                               &encodedSize)
                  .ok());
  std::vector<std::uint8_t> malformed(encoded.begin(),
                                      encoded.begin() + encodedSize);
  auto dataTag = std::find(malformed.rbegin(), malformed.rend(),
                           static_cast<std::uint8_t>(0x83));
  ASSERT_NE(dataTag, malformed.rend());
  *std::prev(dataTag.base()) = 0x95;
  std::uint32_t invokeId = 0;
  IEC61850::MmsWriteRequest decoded;
  EXPECT_FALSE(IEC61850::DecodeMmsWriteRequest(malformed, &invokeId, &decoded)
                   .ok());
  EXPECT_EQ(invokeId, 0u);
  EXPECT_TRUE(decoded.items.empty());

  IEC61850::MmsWriteRequest twoItems = request;
  twoItems.items.push_back(request.items.front());
  std::array<std::uint8_t, 256> twoItemsEncoded{};
  std::size_t twoItemsSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsWriteRequest(
                  1, twoItems, twoItemsEncoded, &twoItemsSize)
                  .ok());
  IEC61850::MmsConfirmedPduView onePdu;
  IEC61850::MmsConfirmedPduView twoPdu;
  ASSERT_TRUE(IEC61850::DecodeMmsConfirmedRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &onePdu)
                  .ok());
  ASSERT_TRUE(IEC61850::DecodeMmsConfirmedRequest(
                  std::span<const std::uint8_t>(twoItemsEncoded.data(),
                                                twoItemsSize),
                  &twoPdu)
                  .ok());
  std::size_t oneOffset = 0;
  IEC61850::BerTlvView ignoredVariables;
  IEC61850::BerTlvView oneData;
  ASSERT_TRUE(IEC61850::ReadBerTlv(onePdu.serviceValue, &oneOffset,
                                   &ignoredVariables)
                  .ok());
  const std::size_t oneDataBegin = oneOffset;
  ASSERT_TRUE(IEC61850::ReadBerTlv(onePdu.serviceValue, &oneOffset, &oneData)
                  .ok());
  EXPECT_EQ(oneData.tag, 0xa0);
  std::size_t twoOffset = 0;
  IEC61850::BerTlvView twoVariables;
  ASSERT_TRUE(IEC61850::ReadBerTlv(twoPdu.serviceValue, &twoOffset,
                                   &twoVariables)
                  .ok());
  EXPECT_EQ(twoVariables.tag, 0xa0);
  std::vector<std::uint8_t> mismatchedService;
  mismatchedService.insert(mismatchedService.end(), twoPdu.serviceValue.begin(),
                           twoPdu.serviceValue.begin() + twoOffset);
  mismatchedService.insert(mismatchedService.end(),
                           onePdu.serviceValue.begin() + oneDataBegin,
                           onePdu.serviceValue.end());
  std::array<std::uint8_t, 256> mismatchedEncoded{};
  std::size_t mismatchedSize = 0;
  ASSERT_TRUE(IEC61850::EncodeMmsConfirmedRequest(
                  1, 5, mismatchedService, mismatchedEncoded, &mismatchedSize)
                  .ok());
  EXPECT_FALSE(IEC61850::DecodeMmsWriteRequest(
                   std::span<const std::uint8_t>(mismatchedEncoded.data(),
                                                 mismatchedSize),
                   &invokeId, &decoded)
                   .ok());
  EXPECT_EQ(invokeId, 0u);
  EXPECT_TRUE(decoded.items.empty());

  IEC61850::MmsWriteResponse invalidResponse;
  invalidResponse.items.push_back({false, 12});
  EXPECT_FALSE(IEC61850::EncodeMmsWriteResponse(
                   1, invalidResponse, encoded, &encodedSize)
                   .ok());
  invalidResponse.items.front().failureCode = -1;
  EXPECT_FALSE(IEC61850::EncodeMmsWriteResponse(
                   1, invalidResponse, encoded, &encodedSize)
                   .ok());

  IEC61850::MmsWriteResponse malformedResponse;
  malformedResponse.items.push_back({true, 0});
  ASSERT_TRUE(IEC61850::EncodeMmsWriteResponse(
                  1, malformedResponse, encoded, &encodedSize)
                  .ok());
  malformed.assign(encoded.begin(), encoded.begin() + encodedSize);
  ASSERT_GE(malformed.size(), 2u);
  malformed.back() = 0x01;
  IEC61850::MmsWriteResponse malformedDecoded;
  EXPECT_FALSE(IEC61850::DecodeMmsWriteResponse(malformed, 1,
                                                 &malformedDecoded)
                   .ok());
}

}  // namespace
