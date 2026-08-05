#include "IEC61850MmsDynamicDataSet.h"
#include "IEC61850MmsFile.h"
#include "IEC61850MmsJournal.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

IEC61850::MmsObjectName Object(std::string identifier) {
  IEC61850::MmsObjectName object;
  object.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
  object.domain = "IED1LD0";
  object.identifier = std::move(identifier);
  return object;
}

std::vector<std::uint8_t> Confirm(std::uint32_t invokeId,
                                  std::uint8_t serviceTag,
                                  std::span<const std::uint8_t> value = {}) {
  std::array<std::uint8_t, 4096> encoded{};
  std::size_t size = 0;
  if (!IEC61850::EncodeMmsConfirmedResponse(invokeId, serviceTag, value,
                                            encoded, &size)
           .ok()) {
    return {};
  }
  return {encoded.begin(), encoded.begin() + size};
}

std::vector<std::uint8_t> SignedResponse(std::uint32_t invokeId,
                                         std::uint8_t serviceTag,
                                         std::int64_t value) {
  if (value < 0 || value > 127) return {};
  const std::array<std::uint8_t, 3> encodedValue = {
      0x02, 0x01, static_cast<std::uint8_t>(value)};
  return Confirm(invokeId, serviceTag, encodedValue);
}

struct ExchangeScript {
  std::vector<std::uint8_t> services;
  std::vector<IEC61850::MmsObjectName> deletedLists;
  std::vector<std::uint8_t> uploaded;
  bool terminated = false;
  std::size_t journalPage = 0;
  std::shared_ptr<std::atomic_bool> cancelAfterInitiate;

  IEC61850::MmsFileExchange Callback() {
    return [this](std::span<const std::uint8_t> request,
                  std::vector<std::uint8_t>* response,
                  std::chrono::milliseconds,
                  const IEC61850::MmsCancellationPredicate& cancelled,
                  const IEC61850::MmsRequestSentHandler&) {
      if (cancelled && cancelled()) {
        return grpc::Status(grpc::StatusCode::CANCELLED, "测试取消");
      }
      IEC61850::MmsConfirmedPduView pdu;
      auto status = IEC61850::DecodeMmsConfirmedRequest(request, &pdu);
      if (!status.ok() || response == nullptr) {
        return grpc::Status(grpc::StatusCode::DATA_LOSS, "测试请求无效");
      }
      services.push_back(pdu.serviceTag);
      if (pdu.serviceTag == 11) {
        IEC61850::MmsNamedVariableListDefinition definition;
        std::uint32_t invokeId = 0;
        status = IEC61850::DecodeMmsDefineNamedVariableListRequest(
            request, &invokeId, &definition);
        if (!status.ok()) return status;
        *response = Confirm(invokeId, 11);
        return grpc::Status::OK;
      }
      if (pdu.serviceTag == 13) {
        IEC61850::MmsObjectName listName;
        std::uint32_t invokeId = 0;
        status = IEC61850::DecodeMmsDeleteNamedVariableListRequest(
            request, &invokeId, &listName);
        if (!status.ok()) return status;
        deletedLists.push_back(listName);
        *response = Confirm(invokeId, 13);
        return grpc::Status::OK;
      }
      if (pdu.serviceTag == 65) {
        IEC61850::MmsJournalReadRequest journalRequest;
        std::uint32_t invokeId = 0;
        status = IEC61850::DecodeMmsJournalReadRequest(request, &invokeId,
                                                        &journalRequest);
        if (!status.ok()) return status;
        IEC61850::MmsJournalReadResponse journalResponse;
        IEC61850::MmsJournalEntry entry;
        entry.entryId = {static_cast<std::uint8_t>(journalPage + 1)};
        entry.occurrenceTimeMs =
            1700000000000LL + static_cast<std::int64_t>(journalPage);
        entry.kind = IEC61850::MmsJournalEntryKind::SOE;
        entry.eventCondition = "Trip";
        journalResponse.entries.push_back(std::move(entry));
        journalResponse.moreFollows = journalPage++ == 0;
        std::array<std::uint8_t, 65536> encoded{};
        std::size_t size = 0;
        status = IEC61850::EncodeMmsJournalReadResponse(
            invokeId, journalResponse, encoded, &size);
        if (!status.ok()) return status;
        *response = {encoded.begin(), encoded.begin() + size};
        return grpc::Status::OK;
      }
      if (pdu.serviceTag == 26) {
        if (cancelAfterInitiate) {
          cancelAfterInitiate->store(true, std::memory_order_release);
        }
        *response = SignedResponse(pdu.invokeId, 26, 42);
        return grpc::Status::OK;
      }
      if (pdu.serviceTag == 27) {
        IEC61850::MmsDownloadSegmentRequest segment;
        std::uint32_t invokeId = 0;
        status = IEC61850::DecodeMmsDownloadSegmentRequest(
            request, &invokeId, &segment);
        if (!status.ok()) return status;
        uploaded.insert(uploaded.end(), segment.data.begin(),
                        segment.data.end());
        *response = Confirm(invokeId, 27);
        return grpc::Status::OK;
      }
      if (pdu.serviceTag == 28) {
        IEC61850::MmsTerminateDownloadRequest terminate;
        std::uint32_t invokeId = 0;
        status = IEC61850::DecodeMmsTerminateDownloadRequest(
            request, &invokeId, &terminate);
        if (!status.ok()) return status;
        terminated = true;
        *response = Confirm(invokeId, 28);
        return grpc::Status::OK;
      }
      *response = Confirm(pdu.invokeId, pdu.serviceTag);
      return grpc::Status::OK;
    };
  }
};


// 验证ReadJournal按EntryID推进分页，并把两页结构化SOE合并到有界输出。
TEST(IEC61850MmsOptionalTransactionTest, ReadsJournalPages) {
  ExchangeScript script;
  std::uint32_t invokeId = 1;
  IEC61850::MmsJournalClient client(script.Callback(), &invokeId);
  IEC61850::MmsJournalReadRequest request;
  request.journal = Object("EventLog");
  IEC61850::MmsJournalReadResponse response;
  const auto journalStatus = client.ReadJournal(
      request, &response, 2, std::chrono::seconds(1));
  ASSERT_TRUE(journalStatus.ok()) << journalStatus.error_message();
  ASSERT_EQ(response.entries.size(), 2U);
  EXPECT_EQ(response.entries[0].kind, IEC61850::MmsJournalEntryKind::SOE);
  EXPECT_EQ(response.entries[1].entryId, (std::vector<std::uint8_t>{2}));
  EXPECT_EQ(script.services, (std::vector<std::uint8_t>{65, 65}));
  EXPECT_EQ(invokeId, 3U);
}

// 验证Journal在调用方取消前不进入交换队列。
TEST(IEC61850MmsOptionalTransactionTest, CancelsJournalBeforeExchange) {
  ExchangeScript script;
  std::uint32_t invokeId = 1;
  IEC61850::MmsJournalClient client(script.Callback(), &invokeId);
  std::atomic_bool cancelled = true;
  IEC61850::MmsJournalStatusResponse response;
  const auto status = client.JournalStatus(
      {Object("EventLog")}, &response, std::chrono::seconds(1),
      [&cancelled] { return cancelled.load(); });
  EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_TRUE(script.services.empty());
}

// 验证动态DataSet在RCB绑定失败时删除已创建的Named Variable List。
TEST(IEC61850MmsOptionalTransactionTest, RollsBackDynamicDataSetBinding) {
  ExchangeScript script;
  std::uint32_t invokeId = 1;
  IEC61850::MmsDynamicDataSetClient client(script.Callback(), &invokeId);
  client.SetCapabilities(true, true);
  IEC61850::MmsNamedVariableListDefinition definition;
  definition.listName = Object("RuntimeDS");
  definition.variables = {Object("X1"), Object("X2")};
  const auto status = client.Create(
      definition,
      [](const auto&) { return grpc::Status::OK; },
      [](const auto&) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "测试RCB绑定失败");
      });
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(script.services, (std::vector<std::uint8_t>{11, 13}));
  ASSERT_EQ(script.deletedLists.size(), 1U);
  EXPECT_EQ(script.deletedLists.front(), definition.listName);
}

// 验证未在Initiate能力中协商动态DataSet时拒绝发送Define请求。
TEST(IEC61850MmsOptionalTransactionTest, RejectsUnsupportedDynamicDataSet) {
  ExchangeScript script;
  std::uint32_t invokeId = 1;
  IEC61850::MmsDynamicDataSetClient client(script.Callback(), &invokeId);
  IEC61850::MmsNamedVariableListDefinition definition;
  definition.listName = Object("RuntimeDS");
  definition.variables = {Object("X1")};
  EXPECT_EQ(client.Create(definition).error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_TRUE(script.services.empty());
}

// 验证Upload按InitiateDownload、DownloadSegment、TerminateDownload顺序发送并合并分片。
TEST(IEC61850MmsOptionalTransactionTest, UploadsSegmentedFileAndTerminates) {
  const std::filesystem::path local = "mms_upload_transaction_test.bin";
  {
    std::ofstream file(local, std::ios::binary | std::ios::trunc);
    file << "hello";
  }
  ExchangeScript script;
  std::uint32_t invokeId = 1;
  IEC61850::MmsFileClient client(script.Callback(), &invokeId);
  IEC61850::MmsFileUploadRequest request;
  request.localFile = local.string();
  request.remoteFile = "/COMTRADE/fault.bin";
  request.remoteDomain = "IED1LD0";
  request.segmentBytes = 2;
  const auto uploadStatus = client.Upload(request, std::chrono::seconds(1));
  ASSERT_TRUE(uploadStatus.ok()) << uploadStatus.error_message();
  EXPECT_EQ(script.services, (std::vector<std::uint8_t>{26, 27, 27, 27, 28}));
  EXPECT_EQ(script.uploaded, (std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o'}));
  EXPECT_TRUE(script.terminated);
  std::filesystem::remove(local);
}

// 验证Upload分片期间取消仍然发送TerminateDownload释放远端句柄。
TEST(IEC61850MmsOptionalTransactionTest, CancellingUploadTerminatesRemoteHandle) {
  const std::filesystem::path local = "mms_upload_cancel_test.bin";
  {
    std::ofstream file(local, std::ios::binary | std::ios::trunc);
    file << "hello";
  }
  ExchangeScript script;
  script.cancelAfterInitiate = std::make_shared<std::atomic_bool>(false);
  std::uint32_t invokeId = 1;
  IEC61850::MmsFileClient client(script.Callback(), &invokeId);
  IEC61850::MmsFileUploadRequest request;
  request.localFile = local.string();
  request.remoteFile = "fault.bin";
  request.cancellation = script.cancelAfterInitiate;
  const auto status = client.Upload(request, std::chrono::seconds(1));
  EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_TRUE(script.terminated);
  std::filesystem::remove(local);
}

}  // namespace
