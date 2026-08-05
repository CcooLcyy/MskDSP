#include "IEC61850MmsFile.h"

#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "IEC61850MmsIsoSession.h"

namespace {

class ScriptedTransport final : public IEC61850::MmsTransport {
public:
  using Handler = std::function<std::vector<std::uint8_t>(
      std::span<const std::uint8_t>)>;

  explicit ScriptedTransport(Handler handler) : handler_(std::move(handler)) {}

  grpc::Status Connect(const IEC61850::MmsTransportEndpoint&,
                       std::uint32_t) override {
    connected_ = true;
    return grpc::Status::OK;
  }

  grpc::Status Send(std::span<const std::uint8_t> payload) override {
    return Send(payload, 0);
  }

  grpc::Status Send(std::span<const std::uint8_t> payload,
                    std::uint32_t) override {
    IEC61850::IsoSessionPduView session;
    auto status = IEC61850::DecodeIsoSessionPdu(payload, &session);
    if (!status.ok() || session.type != IEC61850::IsoSessionPduType::DATA) {
      return grpc::Status(grpc::StatusCode::DATA_LOSS,
                          "测试传输收到非法Session报文");
    }
    std::span<const std::uint8_t> mmsRequest;
    status = IEC61850::DecodeMmsPresentationData(session.userData, &mmsRequest);
    if (!status.ok()) {
      return status;
    }
    auto mmsResponse = handler_(mmsRequest);
    if (mmsResponse.empty()) {
      return grpc::Status(grpc::StatusCode::DATA_LOSS,
                          "测试脚本未生成MMS响应");
    }
    std::array<std::uint8_t, 65536> presentation{};
    std::size_t presentationSize = 0;
    status = IEC61850::EncodeMmsPresentationData(mmsResponse, presentation,
                                                   &presentationSize);
    if (!status.ok()) {
      return status;
    }
    std::array<std::uint8_t, 65536> sessionBuffer{};
    std::size_t sessionSize = 0;
    status = IEC61850::EncodeIsoSessionData(
        std::span<const std::uint8_t>(presentation.data(), presentationSize),
        sessionBuffer, &sessionSize);
    if (!status.ok()) {
      return status;
    }
    responses_.emplace_back(sessionBuffer.begin(),
                            sessionBuffer.begin() + sessionSize);
    return grpc::Status::OK;
  }

  grpc::Status Receive(std::vector<std::uint8_t>* payload,
                       std::uint32_t) override {
    if (payload == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "测试传输输出为空");
    }
    if (responses_.empty()) {
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                          "测试传输没有脚本响应");
    }
    *payload = std::move(responses_.front());
    responses_.pop_front();
    return grpc::Status::OK;
  }

  void Close() noexcept override { connected_ = false; }
  bool IsConnected() const noexcept override { return connected_; }

private:
  Handler handler_;
  std::deque<std::vector<std::uint8_t>> responses_;
  bool connected_ = true;
};

std::vector<std::uint8_t> EncodeCloseResponse(std::uint32_t invokeId) {
  std::array<std::uint8_t, 4096> encoded{};
  std::size_t size = 0;
  if (!IEC61850::EncodeMmsConfirmedResponse(invokeId, 74, {}, encoded, &size)
           .ok()) {
    return {};
  }
  return {encoded.begin(), encoded.begin() + size};
}

std::vector<std::uint8_t> EncodeDirectoryResponse(std::uint32_t invokeId,
                                                  std::string_view name,
                                                  std::uint64_t size,
                                                  bool more) {
  IEC61850::MmsFileDirectoryResponse response;
  response.moreFollows = more;
  IEC61850::MmsFileDirectoryEntry entry;
  entry.fileName = std::string(name);
  entry.fileSize = size;
  entry.attributes.sizePresent = true;
  entry.attributes.size = size;
  response.entries.push_back(std::move(entry));
  std::array<std::uint8_t, 4096> encoded{};
  std::size_t encodedSize = 0;
  if (!IEC61850::EncodeMmsFileDirectoryResponse(invokeId, response, encoded,
                                                 &encodedSize)
           .ok()) {
    return {};
  }
  return {encoded.begin(), encoded.begin() + encodedSize};
}

std::vector<std::uint8_t> EncodeOpenResponse(std::uint32_t invokeId,
                                             std::int32_t frsmId,
                                             std::uint64_t size) {
  IEC61850::MmsFileOpenResponse response;
  response.frsmId = frsmId;
  response.fileSize = size;
  response.attributes.sizePresent = true;
  response.attributes.size = size;
  std::array<std::uint8_t, 4096> encoded{};
  std::size_t encodedSize = 0;
  if (!IEC61850::EncodeMmsFileOpenResponse(invokeId, response, encoded,
                                            &encodedSize)
           .ok()) {
    return {};
  }
  return {encoded.begin(), encoded.begin() + encodedSize};
}

std::vector<std::uint8_t> EncodeReadResponse(std::uint32_t invokeId,
                                             std::string_view data,
                                             bool more) {
  IEC61850::MmsFileReadResponse response;
  response.data.assign(data.begin(), data.end());
  response.fileData = response.data;
  response.moreFollows = more;
  std::array<std::uint8_t, 4096> encoded{};
  std::size_t encodedSize = 0;
  if (!IEC61850::EncodeMmsFileReadResponse(invokeId, response, encoded,
                                            &encodedSize)
           .ok()) {
    return {};
  }
  return {encoded.begin(), encoded.begin() + encodedSize};
}

}  // namespace

// 验证FileDirectory、FileOpen以及primitive FileRead/FileClose请求的高标签和字段往返。
TEST(IEC61850MmsFileTest, EncodesAndDecodesRequests) {
  std::array<std::uint8_t, 4096> encoded{};
  std::size_t encodedSize = 0;
  IEC61850::MmsFileDirectoryRequest directory;
  directory.fileSpecification = "COMTRADE";
  directory.continueAfter = "a.cfg";
  ASSERT_TRUE(IEC61850::EncodeMmsFileDirectoryRequest(
                  7, directory, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsFileDirectoryRequest decodedDirectory;
  std::uint32_t invokeId = 0;
  ASSERT_TRUE(IEC61850::DecodeMmsFileDirectoryRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &decodedDirectory)
                  .ok());
  EXPECT_EQ(invokeId, 7U);
  EXPECT_EQ(decodedDirectory.fileSpecification, "COMTRADE");
  EXPECT_EQ(decodedDirectory.continueAfter, "a.cfg");

  IEC61850::MmsFileOpenRequest open;
  open.fileName = "fault.cfg";
  open.initialPosition = 19;
  ASSERT_TRUE(IEC61850::EncodeMmsFileOpenRequest(8, open, encoded,
                                                  &encodedSize)
                  .ok());
  IEC61850::MmsFileOpenRequest decodedOpen;
  ASSERT_TRUE(IEC61850::DecodeMmsFileOpenRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &decodedOpen)
                  .ok());
  EXPECT_EQ(decodedOpen.fileName, open.fileName);
  EXPECT_EQ(decodedOpen.initialPosition, open.initialPosition);

  ASSERT_TRUE(IEC61850::EncodeMmsFileReadRequest(9, 19, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsFileReadRequest decodedRead;
  ASSERT_TRUE(IEC61850::DecodeMmsFileReadRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &decodedRead)
                  .ok());
  EXPECT_EQ(decodedRead.frsmId, 19);

  ASSERT_TRUE(IEC61850::EncodeMmsFileCloseRequest(10, 19, encoded, &encodedSize)
                  .ok());
  IEC61850::MmsFileCloseRequest decodedClose;
  ASSERT_TRUE(IEC61850::DecodeMmsFileCloseRequest(
                  std::span<const std::uint8_t>(encoded.data(), encodedSize),
                  &invokeId, &decodedClose)
                  .ok());
  EXPECT_EQ(decodedClose.frsmId, 19);
}

// 验证FileDirectory分页会更新continueAfter并拒绝空后续分页。
TEST(IEC61850MmsFileTest, ReadsDirectoryPages) {
  std::vector<std::string> continueAfter;
  std::vector<std::string> specifications;
  ScriptedTransport transport([&continueAfter, &specifications](
                                  std::span<const std::uint8_t> raw) {
    IEC61850::MmsConfirmedPduView pdu;
    if (!IEC61850::DecodeMmsConfirmedRequest(raw, &pdu).ok()) {
      return std::vector<std::uint8_t>{};
    }
    IEC61850::MmsFileDirectoryRequest request;
    std::uint32_t invokeId = 0;
    if (!IEC61850::DecodeMmsFileDirectoryRequest(raw, &invokeId, &request)
             .ok()) {
      return std::vector<std::uint8_t>{};
    }
    specifications.push_back(request.fileSpecification);
    continueAfter.push_back(request.continueAfter.value_or(""));
    return continueAfter.size() == 1
               ? EncodeDirectoryResponse(invokeId, "a.cfg", 3, true)
               : EncodeDirectoryResponse(invokeId, "b.cfg", 4, false);
  });
  std::uint32_t nextInvokeId = 1;
  IEC61850::MmsFileClient client;
  IEC61850::MmsFileDirectoryRequest request;
  request.fileSpecification = "/COMTRADE/";
  IEC61850::MmsFileDirectoryResponse response;
  ASSERT_TRUE(client.ReadDirectory(transport, &nextInvokeId, request, &response,
                                   std::chrono::seconds(1))
                  .ok());
  ASSERT_EQ(response.entries.size(), 2U);
  EXPECT_EQ(response.entries[0].fileName, "a.cfg");
  EXPECT_EQ(response.entries[1].fileName, "b.cfg");
  EXPECT_EQ(continueAfter, (std::vector<std::string>{"", "a.cfg"}));
  EXPECT_EQ(specifications,
            (std::vector<std::string>{"/COMTRADE/", "/COMTRADE/"}));
}

// 验证Download按Open、多个Read、Close顺序合并分片并原子落盘。
TEST(IEC61850MmsFileTest, DownloadsSegmentedFile) {
  std::vector<std::uint32_t> services;
  ScriptedTransport transport([&services](std::span<const std::uint8_t> raw) {
    IEC61850::MmsConfirmedPduView pdu;
    if (!IEC61850::DecodeMmsConfirmedRequest(raw, &pdu).ok()) {
      return std::vector<std::uint8_t>{};
    }
    services.push_back(pdu.serviceTag);
    if (pdu.serviceTag == 72) {
      return EncodeOpenResponse(pdu.invokeId, 27, 5);
    }
    if (pdu.serviceTag == 73) {
      return services.size() == 2
                 ? EncodeReadResponse(pdu.invokeId, "hel", true)
                 : EncodeReadResponse(pdu.invokeId, "lo", false);
    }
    if (pdu.serviceTag == 74) {
      return EncodeCloseResponse(pdu.invokeId);
    }
    return std::vector<std::uint8_t>{};
  });
  const std::filesystem::path local =
      std::filesystem::absolute("mms_file_test.cfg");
  std::filesystem::remove(local);
  IEC61850::MmsFileDownloadRequest request;
  request.remoteFile = "/COMTRADE/fault.cfg";
  request.localFile = local.string();
  request.maxBytes = 16;
  IEC61850::MmsFileClient client;
  std::uint32_t nextInvokeId = 1;
  ASSERT_TRUE(client.Download(transport, &nextInvokeId, request,
                              std::chrono::seconds(1))
                  .ok());
  EXPECT_EQ(services, (std::vector<std::uint32_t>{72, 73, 73, 74}));
  EXPECT_EQ(std::filesystem::file_size(local), 5U);
  std::filesystem::remove(local);
}

// 验证空文件仍执行一次FileRead并完成FileClose，不把零长度内容误判为截断。
TEST(IEC61850MmsFileTest, DownloadsEmptyFile) {
  std::vector<std::uint32_t> services;
  ScriptedTransport transport([&services](std::span<const std::uint8_t> raw) {
    IEC61850::MmsConfirmedPduView pdu;
    if (!IEC61850::DecodeMmsConfirmedRequest(raw, &pdu).ok()) {
      return std::vector<std::uint8_t>{};
    }
    services.push_back(pdu.serviceTag);
    if (pdu.serviceTag == 72) {
      return EncodeOpenResponse(pdu.invokeId, 31, 0);
    }
    if (pdu.serviceTag == 73) {
      return EncodeReadResponse(pdu.invokeId, {}, false);
    }
    if (pdu.serviceTag == 74) {
      return EncodeCloseResponse(pdu.invokeId);
    }
    return std::vector<std::uint8_t>{};
  });
  const std::filesystem::path local = "mms_empty_file_test.cfg";
  std::filesystem::remove(local);
  IEC61850::MmsFileDownloadRequest request;
  request.remoteFile = "empty.cfg";
  request.localFile = local.string();
  IEC61850::MmsFileClient client;
  std::uint32_t nextInvokeId = 1;
  ASSERT_TRUE(client.Download(transport, &nextInvokeId, request,
                              std::chrono::seconds(1))
                  .ok());
  EXPECT_EQ(services, (std::vector<std::uint32_t>{72, 73, 74}));
  EXPECT_EQ(std::filesystem::file_size(local), 0U);
  std::filesystem::remove(local);
}

// 验证AR502H路径格式可用，同时继续拒绝目录穿越、空路径组件和目录目标。
TEST(IEC61850MmsFileTest, RejectsUnsafePaths) {
  EXPECT_TRUE(IEC61850::MmsFileClient::ValidateRemoteFileName("/COMTRADE/")
                  .ok());
  EXPECT_TRUE(
      IEC61850::MmsFileClient::ValidateRemoteFileName("/COMTRADE/fault.cfg")
          .ok());
  EXPECT_TRUE(IEC61850::MmsFileClient::ValidateRemoteFileName(
                  "\\COMTRADE\\fault.cfg")
                  .ok());
  EXPECT_FALSE(IEC61850::MmsFileClient::ValidateRemoteFileName("../fault.cfg")
                   .ok());
  EXPECT_FALSE(IEC61850::MmsFileClient::ValidateRemoteFileName(
                   "/COMTRADE//fault.cfg")
                   .ok());
  EXPECT_FALSE(IEC61850::MmsFileClient::ValidateRemoteFileName(
                   "/COMTRADE//")
                   .ok());
  EXPECT_FALSE(IEC61850::MmsFileClient::ValidateRemoteFileName("/").ok());
  EXPECT_TRUE(IEC61850::MmsFileClient::ValidateLocalFileName("/tmp/fault.cfg")
                  .ok());
  EXPECT_FALSE(IEC61850::MmsFileClient::ValidateLocalFileName("../fault.cfg")
                   .ok());
  EXPECT_FALSE(IEC61850::MmsFileClient::ValidateLocalFileName("/tmp/")
                   .ok());
}
