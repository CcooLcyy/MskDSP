#include "IEC61850MmsExchange.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <limits>
#include <string>

#include "IEC61850MmsIsoSession.h"
#include "IEC61850MmsService.h"
#include "Logger.h"

namespace IEC61850 {
namespace {

constexpr std::uint32_t kMmsIoTimeoutMs = 1000;
constexpr std::uint32_t kMmsCancellationPollMs = 50;
constexpr std::uint32_t kMmsConfirmedExchangeTimeoutMs = 5000;
constexpr std::size_t kMmsPduBufferSize = 4096;

std::uint32_t RemainingTimeoutMs(std::chrono::steady_clock::time_point deadline) {
  const auto remaining = deadline - std::chrono::steady_clock::now();
  if (remaining <= std::chrono::steady_clock::duration::zero()) {
    return 0;
  }
  auto timeout =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  if (timeout <= std::chrono::milliseconds::zero()) {
    timeout = std::chrono::milliseconds(1);
  } else if (timeout < remaining) {
    ++timeout;
  }
  return static_cast<std::uint32_t>(std::min<std::int64_t>(
      timeout.count(),
      static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())));
}

std::string HexDump(std::span<const std::uint8_t> bytes) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 3);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index != 0) {
      result.push_back(' ');
    }
    result.push_back(kHex[bytes[index] >> 4]);
    result.push_back(kHex[bytes[index] & 0x0f]);
  }
  return result;
}

}  // namespace

grpc::Status ExchangeMmsConfirmedRequest(
    MmsTransport& transport, std::span<const std::uint8_t> request,
    std::vector<std::uint8_t>* response,
    const MmsUnconfirmedHandler& onUnconfirmed,
    std::optional<std::chrono::milliseconds> timeout,
    const MmsCancellationPredicate& isCancelled,
    const MmsRequestSentHandler& onRequestSent) {
  if (response == nullptr || request.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS确认服务交换参数无效");
  }
  response->clear();
  MmsConfirmedPduView requestPdu;
  auto status = DecodeMmsConfirmedRequest(request, &requestPdu);
  if (!status.ok() || requestPdu.invokeId == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS确认请求缺少有效invokeID");
  }
  const auto expectedInvokeId = requestPdu.invokeId;
  std::array<std::uint8_t, kMmsPduBufferSize> presentationBuffer{};
  std::size_t presentationSize = 0;
  status = EncodeMmsPresentationData(request, presentationBuffer,
                                     &presentationSize);
  if (!status.ok()) {
    return status;
  }
  std::array<std::uint8_t, kMmsPduBufferSize> sessionBuffer{};
  std::size_t sessionSize = 0;
  status = EncodeIsoSessionData(
      std::span<const std::uint8_t>(presentationBuffer.data(),
                                    presentationSize),
      sessionBuffer, &sessionSize);
  if (!status.ok()) {
    return status;
  }
  const auto exchangeTimeout = timeout.value_or(
      std::chrono::milliseconds(kMmsConfirmedExchangeTimeoutMs));
  if (isCancelled && isCancelled()) {
    LOG_WARNING("IEC61850 MMS确认服务发送前已取消");
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS确认服务已取消");
  }
  if (exchangeTimeout <= std::chrono::milliseconds::zero()) {
    LOG_WARNING("IEC61850 MMS确认服务截止时间已耗尽: 超时={}毫秒",
                exchangeTimeout.count());
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS确认服务等待时间已耗尽");
  }
  const auto deadline = std::chrono::steady_clock::now() + exchangeTimeout;
  if (std::chrono::steady_clock::now() >= deadline) {
    LOG_WARNING("IEC61850 MMS确认服务发送前已超过截止时间: 超时={}毫秒",
                exchangeTimeout.count());
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS确认服务等待时间已耗尽");
  }
  // 取消检查与Send之间的调用点构成本次交换的发送线性化边界；
  // 回调返回false时，调用方已经在边界前取消，不能再把请求交给传输层。
  if (onRequestSent && !onRequestSent()) {
    LOG_WARNING("IEC61850 MMS确认服务在发送线性化边界前已取消");
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS确认服务已取消");
  }
  const auto sendTimeoutMs = RemainingTimeoutMs(deadline);
  if (sendTimeoutMs == 0) {
    LOG_WARNING("IEC61850 MMS确认服务发送前已耗尽剩余截止时间");
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS确认服务等待时间已耗尽");
  }
  status = transport.Send(
      std::span<const std::uint8_t>(sessionBuffer.data(), sessionSize),
      sendTimeoutMs);
  if (!status.ok()) {
    return status;
  }
  LOG_DEBUG("IEC61850 MMS发送Confirmed PDU: {}", HexDump(request));

  if (isCancelled && isCancelled()) {
    LOG_WARNING("IEC61850 MMS确认服务已发送后收到取消");
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS确认服务已取消");
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    LOG_WARNING("IEC61850 MMS确认服务发送完成后已超过截止时间: 超时={}毫秒",
                exchangeTimeout.count());
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS确认服务等待响应超时");
  }
  for (;;) {
    if (isCancelled && isCancelled()) {
      LOG_WARNING("IEC61850 MMS确认服务等待期间已取消");
      return grpc::Status(grpc::StatusCode::CANCELLED,
                          "IEC61850 MMS确认服务已取消");
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      LOG_WARNING("IEC61850 MMS确认服务等待响应超时: 超时={}毫秒",
                  exchangeTimeout.count());
      break;
    }
    const auto ioPollMs = isCancelled ? kMmsCancellationPollMs : kMmsIoTimeoutMs;
    const auto remainingMs = RemainingTimeoutMs(deadline);
    const auto timeoutMs = std::min(ioPollMs, remainingMs);
    if (timeoutMs == 0) {
      break;
    }
    std::vector<std::uint8_t> received;
    status = transport.Receive(&received, timeoutMs);
    if (!status.ok()) {
      if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED &&
          std::chrono::steady_clock::now() < deadline) {
        continue;
      }
      return status;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      LOG_WARNING("IEC61850 MMS确认服务收到迟到响应，按超时处理");
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                          "IEC61850 MMS确认服务收到迟到响应");
    }
    IsoSessionPduView sessionPdu;
    status = DecodeIsoSessionPdu(received, &sessionPdu);
    if (!status.ok()) {
      return status;
    }
    if (sessionPdu.type != IsoSessionPduType::DATA) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "IEC61850 MMS确认服务未收到DATA响应");
    }
    std::span<const std::uint8_t> mmsResponse;
    status = DecodeMmsPresentationData(sessionPdu.userData, &mmsResponse);
    if (!status.ok()) {
      return status;
    }
    LOG_DEBUG("IEC61850 MMS接收待分派PDU: 报文={}", HexDump(mmsResponse));
    if (!mmsResponse.empty() && mmsResponse.front() == 0xa3) {
      if (onUnconfirmed) {
        onUnconfirmed(mmsResponse);
      } else {
        LOG_WARNING("IEC61850 MMS确认请求期间收到未确认PDU但没有入队处理器: 报文={}",
                    HexDump(mmsResponse));
      }
      continue;
    }
    if (!mmsResponse.empty() && mmsResponse.front() == 0xa2) {
      MmsConfirmedErrorPduView remoteError;
      status = DecodeMmsConfirmedError(mmsResponse, &remoteError);
      if (!status.ok()) {
        return status;
      }
      if (remoteError.invokeId != expectedInvokeId) {
        LOG_WARNING(
            "IEC61850 MMS忽略invokeID不匹配的Confirmed-ErrorPDU: 期望={}, 实际={}, 报文={}",
            expectedInvokeId, remoteError.invokeId, HexDump(mmsResponse));
        continue;
      }
      std::string message = std::format(
          "IEC61850 MMS服务端返回Confirmed-ErrorPDU: invokeID={}, 错误类={}, 错误码={}",
          remoteError.invokeId, remoteError.errorClass, remoteError.errorCode);
      if (remoteError.additionalCode.has_value()) {
        message += std::format("，附加码={}", *remoteError.additionalCode);
      }
      if (!remoteError.additionalDescription.empty()) {
        message += std::format("，描述={}", remoteError.additionalDescription);
      }
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          std::move(message));
    }
    MmsConfirmedPduView responsePdu;
    status = DecodeMmsConfirmedResponse(mmsResponse, &responsePdu);
    if (!status.ok()) {
      return status;
    }
    if (responsePdu.invokeId != expectedInvokeId) {
      LOG_WARNING("IEC61850 MMS忽略invokeID不匹配的确认响应: 期望={}, 实际={}, 报文={}",
                  expectedInvokeId, responsePdu.invokeId,
                  HexDump(mmsResponse));
      continue;
    }
    response->assign(mmsResponse.begin(), mmsResponse.end());
    LOG_DEBUG("IEC61850 MMS接收匹配Confirmed PDU: invokeID={}, 报文={}",
              expectedInvokeId, HexDump(mmsResponse));
    return grpc::Status::OK;
  }
  return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                      "IEC61850 MMS确认请求期间未收到匹配响应");
}

}  // namespace IEC61850
