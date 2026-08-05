#include "IEC61850MmsExchange.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "IEC61850MmsIsoSession.h"
#include "IEC61850MmsService.h"

namespace {

using namespace std::chrono_literals;

class ScriptedMmsTransport final : public IEC61850::MmsTransport {
public:
  grpc::Status Connect(const IEC61850::MmsTransportEndpoint&,
                       std::uint32_t = 0) override {
    connected_ = true;
    return grpc::Status::OK;
  }

  grpc::Status Send(std::span<const std::uint8_t> payload) override {
    if (!connected_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "测试MMS传输尚未连接");
    }
    sent.emplace_back(payload.begin(), payload.end());
    return grpc::Status::OK;
  }

  grpc::Status Send(std::span<const std::uint8_t> payload,
                    std::uint32_t timeoutMs) override {
    lastSendTimeout = timeoutMs;
    return Send(payload);
  }

  grpc::Status Receive(std::vector<std::uint8_t>* payload,
                       std::uint32_t) override {
    if (payload == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "测试MMS接收输出为空");
    }
    if (!connected_) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "测试MMS传输尚未连接");
    }
    if (received.empty()) {
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                          "测试MMS脚本已经耗尽");
    }
    if (receiveDelay > std::chrono::milliseconds::zero()) {
      std::this_thread::sleep_for(receiveDelay);
    }
    *payload = std::move(received.front());
    received.pop_front();
    return grpc::Status::OK;
  }

  void Close() noexcept override { connected_ = false; }

  bool IsConnected() const noexcept override { return connected_; }

  bool connected_ = true;
  std::deque<std::vector<std::uint8_t>> received;
  std::vector<std::vector<std::uint8_t>> sent;
  std::chrono::milliseconds receiveDelay{};
  std::uint32_t lastSendTimeout = 0;
};

std::vector<std::uint8_t> EncodeConfirmed(std::uint8_t outerTag,
                                          std::uint32_t invokeId) {
  std::array<std::uint8_t, 64> encoded{};
  std::size_t encodedSize = 0;
  grpc::Status status;
  if (outerTag == 0xa0) {
    status = IEC61850::EncodeMmsConfirmedRequest(
        invokeId, 1, {}, encoded, &encodedSize);
  } else {
    status = IEC61850::EncodeMmsConfirmedResponse(
        invokeId, 1, {}, encoded, &encodedSize);
  }
  EXPECT_TRUE(status.ok());
  return {encoded.begin(), encoded.begin() + encodedSize};
}

std::vector<std::uint8_t> EncodeConfirmedError(std::uint32_t invokeId) {
  constexpr std::array<std::uint8_t, 21> encoded{
      0xa2, 0x13, 0x02, 0x01, 0x07, 0x30, 0x0e, 0x87, 0x01, 0x03,
      0x81, 0x01, 0x2a, 0x82, 0x06, 'd',  'e',  'n',  'i',  'e',  'd'};
  auto result = std::vector<std::uint8_t>(encoded.begin(), encoded.end());
  result[4] = static_cast<std::uint8_t>(invokeId);
  return result;
}

std::vector<std::uint8_t> WrapSessionData(
    std::span<const std::uint8_t> mmsPdu) {
  std::array<std::uint8_t, 256> presentation{};
  std::size_t presentationSize = 0;
  EXPECT_TRUE(IEC61850::EncodeMmsPresentationData(
                  mmsPdu, presentation, &presentationSize)
                  .ok());
  std::array<std::uint8_t, 512> session{};
  std::size_t sessionSize = 0;
  EXPECT_TRUE(IEC61850::EncodeIsoSessionData(
                  std::span<const std::uint8_t>(presentation.data(),
                                                presentationSize),
                  session, &sessionSize)
                  .ok());
  return {session.begin(), session.begin() + sessionSize};
}

}  // namespace

// 验证异步报告和其他invokeID响应先到时，交换器仍只交付当前请求的匹配响应。
TEST(IEC61850MmsExchangeTest, DispatchesInterleavedPdusByInvokeId) {
  ScriptedMmsTransport transport;
  const auto request = EncodeConfirmed(0xa0, 7);
  const std::vector<std::uint8_t> informationReport{0xa3, 0x00};
  const auto unrelatedResponse = EncodeConfirmed(0xa1, 8);
  const auto expectedResponse = EncodeConfirmed(0xa1, 7);
  transport.received.push_back(WrapSessionData(informationReport));
  transport.received.push_back(WrapSessionData(unrelatedResponse));
  transport.received.push_back(WrapSessionData(expectedResponse));

  std::vector<std::vector<std::uint8_t>> unconfirmed;
  std::vector<std::uint8_t> response;
  const auto status = IEC61850::ExchangeMmsConfirmedRequest(
      transport, request, &response,
      [&](std::span<const std::uint8_t> pdu) {
        unconfirmed.emplace_back(pdu.begin(), pdu.end());
      });

  ASSERT_TRUE(status.ok());
  ASSERT_EQ(unconfirmed.size(), 1u);
  EXPECT_EQ(unconfirmed.front(), informationReport);
  EXPECT_EQ(response, expectedResponse);
  ASSERT_EQ(transport.sent.size(), 1u);

  IEC61850::IsoSessionPduView sessionPdu;
  ASSERT_TRUE(IEC61850::DecodeIsoSessionPdu(transport.sent.front(),
                                            &sessionPdu)
                  .ok());
  ASSERT_EQ(sessionPdu.type, IEC61850::IsoSessionPduType::DATA);
  std::span<const std::uint8_t> sentRequest;
  ASSERT_TRUE(IEC61850::DecodeMmsPresentationData(sessionPdu.userData,
                                                  &sentRequest)
                  .ok());
  ASSERT_EQ(sentRequest.size(), request.size());
  EXPECT_TRUE(std::equal(sentRequest.begin(), sentRequest.end(),
                         request.begin()));
}

// 验证超过32份连续异步报告到达时，仍会继续等待当前invokeID的确认响应。
TEST(IEC61850MmsExchangeTest, AllowsLargeInterleavedReportBurst) {
  ScriptedMmsTransport transport;
  const auto request = EncodeConfirmed(0xa0, 9);
  const std::vector<std::uint8_t> informationReport{0xa3, 0x00};
  for (int index = 0; index < 40; ++index) {
    transport.received.push_back(WrapSessionData(informationReport));
  }
  const auto expectedResponse = EncodeConfirmed(0xa1, 9);
  transport.received.push_back(WrapSessionData(expectedResponse));

  std::size_t unconfirmedCount = 0;
  std::vector<std::uint8_t> response;
  const auto status = IEC61850::ExchangeMmsConfirmedRequest(
      transport, request, &response,
      [&](std::span<const std::uint8_t> pdu) {
        ASSERT_EQ(pdu.size(), informationReport.size());
        EXPECT_TRUE(std::equal(pdu.begin(), pdu.end(), informationReport.begin()));
        ++unconfirmedCount;
      });

  ASSERT_TRUE(status.ok());
  EXPECT_EQ(unconfirmedCount, 40u);
  EXPECT_EQ(response, expectedResponse);
}

// 验证匹配invokeID的Confirmed-ErrorPDU被映射为远端服务拒绝，而不是报文格式错误。
TEST(IEC61850MmsExchangeTest, MapsConfirmedErrorPduToRemoteServiceFailure) {
  ScriptedMmsTransport transport;
  const auto request = EncodeConfirmed(0xa0, 11);
  transport.received.push_back(WrapSessionData(EncodeConfirmedError(11)));

  std::vector<std::uint8_t> response;
  const auto status = IEC61850::ExchangeMmsConfirmedRequest(
      transport, request, &response);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("错误类=7"), std::string::npos);
  EXPECT_NE(status.error_message().find("错误码=3"), std::string::npos);
  EXPECT_TRUE(response.empty());
}

// 验证invokeID不匹配的Confirmed-ErrorPDU会被忽略并继续等待目标响应。
TEST(IEC61850MmsExchangeTest, IgnoresMismatchedConfirmedErrorPdu) {
  ScriptedMmsTransport transport;
  const auto request = EncodeConfirmed(0xa0, 12);
  transport.received.push_back(WrapSessionData(EncodeConfirmedError(13)));
  const auto expectedResponse = EncodeConfirmed(0xa1, 12);
  transport.received.push_back(WrapSessionData(expectedResponse));

  std::vector<std::uint8_t> response;
  const auto status = IEC61850::ExchangeMmsConfirmedRequest(
      transport, request, &response);

  ASSERT_TRUE(status.ok());
  EXPECT_EQ(response, expectedResponse);
}

// 验证无效确认请求在触发网络发送前被拒绝，且输出不会保留旧响应。
TEST(IEC61850MmsExchangeTest, RejectsInvalidRequestBeforeSending) {
  ScriptedMmsTransport transport;
  std::vector<std::uint8_t> response{0xff};
  const std::array<std::uint8_t, 2> malformed{0xa0, 0x00};

  const auto status = IEC61850::ExchangeMmsConfirmedRequest(
      transport, malformed, &response);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(response.empty());
  EXPECT_TRUE(transport.sent.empty());
}

// 验证确认服务在发送前已取消时不会向IED发送MMS控制报文。
TEST(IEC61850MmsExchangeTest, RejectsCancellationBeforeSending) {
  ScriptedMmsTransport transport;
  const auto request = EncodeConfirmed(0xa0, 15);
  std::vector<std::uint8_t> response;

  const auto status = IEC61850::ExchangeMmsConfirmedRequest(
      transport, request, &response, {}, std::chrono::milliseconds(500),
      [] { return true; });

  EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_TRUE(response.empty());
  EXPECT_TRUE(transport.sent.empty());
}

// 验证发送线性化回调在取消竞态中可以阻止请求进入传输层。
TEST(IEC61850MmsExchangeTest, RejectsCancellationAtSendLinearizationBoundary) {
  ScriptedMmsTransport transport;
  const auto request = EncodeConfirmed(0xa0, 17);
  std::vector<std::uint8_t> response;

  const auto status = IEC61850::ExchangeMmsConfirmedRequest(
      transport, request, &response, {}, std::chrono::milliseconds(500), {},
      [] { return false; });

  EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_TRUE(response.empty());
  EXPECT_TRUE(transport.sent.empty());
}

// 验证确认服务已发送请求后收到取消时立即结束等待，不伪造成功响应。
TEST(IEC61850MmsExchangeTest, CancelsWhileWaitingForConfirmedResponse) {
  ScriptedMmsTransport transport;
  const auto request = EncodeConfirmed(0xa0, 16);
  std::vector<std::uint8_t> response;
  std::size_t checks = 0;

  const auto status = IEC61850::ExchangeMmsConfirmedRequest(
      transport, request, &response, {}, std::chrono::milliseconds(500),
      [&checks] { return ++checks >= 2; });

  EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_TRUE(response.empty());
  ASSERT_EQ(transport.sent.size(), 1u);
}

// 验证传输层在调用截止时间之后才返回有效响应时，交换器仍拒绝迟到成功。
TEST(IEC61850MmsExchangeTest, RejectsResponseReturnedAfterDeadline) {
  ScriptedMmsTransport transport;
  transport.receiveDelay = 30ms;
  const auto request = EncodeConfirmed(0xa0, 18);
  transport.received.push_back(WrapSessionData(EncodeConfirmed(0xa1, 18)));

  std::vector<std::uint8_t> response;
  const auto status = IEC61850::ExchangeMmsConfirmedRequest(
      transport, request, &response, {}, 5ms);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_TRUE(response.empty());
  EXPECT_GT(transport.lastSendTimeout, 0u);
  EXPECT_LE(transport.lastSendTimeout, 5u);
}
