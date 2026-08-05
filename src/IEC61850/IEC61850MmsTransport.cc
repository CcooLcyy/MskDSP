#include "IEC61850MmsTransport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <limits>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Logger.h"

namespace IEC61850 {
namespace {

constexpr std::size_t kTpktHeaderSize = 4;
constexpr std::size_t kCotpDataHeaderSize = 3;
constexpr std::size_t kCotpConnectionRequestSize = 22;
constexpr std::size_t kMinimumCotpDataFrameSize =
    kTpktHeaderSize + kCotpDataHeaderSize;
constexpr std::size_t kCotpSegmentPayloadSize = 1017;
constexpr std::size_t kMaximumReassembledPayloadSize = 4 * 1024 * 1024;

using SteadyClock = std::chrono::steady_clock;
using Deadline = SteadyClock::time_point;

std::uint32_t DefaultConnectBudget(const MmsTransportEndpoint& endpoint) {
  const auto total = static_cast<std::uint64_t>(endpoint.connectTimeoutMs) +
                     static_cast<std::uint64_t>(endpoint.ioTimeoutMs) * 2u;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(
      total, std::numeric_limits<std::uint32_t>::max()));
}

std::uint16_t ReadNetworkUint16(const std::uint8_t* bytes) noexcept {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[0]) << 8) | bytes[1]);
}

void WriteNetworkUint16(std::uint8_t* bytes, std::uint16_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>(value >> 8);
  bytes[1] = static_cast<std::uint8_t>(value);
}

std::string HexDump(std::span<const std::uint8_t> bytes) {
  std::string result;
  result.reserve(bytes.size() * 3);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index != 0) {
      result.push_back(' ');
    }
    result += std::format("{:02x}", bytes[index]);
  }
  return result;
}

grpc::Status InvalidFrame(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::DATA_LOSS,
                      std::format("IEC61850 MMS传输帧无效: {}", reason));
}

grpc::Status SystemError(std::string_view operation,
                         grpc::StatusCode code = grpc::StatusCode::UNAVAILABLE) {
  const auto error = errno;
  return grpc::Status(code,
                      std::format("{}失败: {}", operation,
                                   std::strerror(error)));
}

grpc::Status ValidateTpktHeader(std::span<const std::uint8_t> frame) {
  if (frame.size() < kTpktHeaderSize) {
    return InvalidFrame("TPKT头长度不足");
  }
  if (frame[0] != 0x03 || frame[1] != 0x00) {
    return InvalidFrame("TPKT版本或保留字段错误");
  }
  const auto length = ReadNetworkUint16(frame.data() + 2);
  if (length != frame.size()) {
    return InvalidFrame("TPKT声明长度与实际长度不一致");
  }
  if (length < kMinimumCotpDataFrameSize) {
    return InvalidFrame("TPKT长度小于COTP最小数据帧");
  }
  return grpc::Status::OK;
}

grpc::Status WaitForSocketUntil(int fd, short events, Deadline deadline) {
  pollfd descriptor{.fd = fd, .events = events, .revents = 0};
  for (;;) {
    const auto now = SteadyClock::now();
    if (now >= deadline) {
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                          "IEC61850 MMS TCP操作超时");
    }
    const auto remaining = deadline - now;
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        remaining);
    if (timeout <= std::chrono::milliseconds::zero()) {
      timeout = std::chrono::milliseconds(1);
    } else if (timeout < remaining) {
      ++timeout;
    }
    const auto boundedTimeout = std::min<std::int64_t>(
        timeout.count(), static_cast<std::int64_t>(std::numeric_limits<int>::max()));
    descriptor.revents = 0;
    const auto result = poll(&descriptor, 1, static_cast<int>(boundedTimeout));
    if (result > 0) {
      if ((descriptor.revents & events) != 0) {
        return grpc::Status::OK;
      }
      return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                          "IEC61850 MMS TCP连接被对端关闭或发生套接字错误");
    }
    if (result == 0) {
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                          "IEC61850 MMS TCP操作超时");
    }
    if (errno == EINTR) {
      continue;
    }
    return SystemError("IEC61850 MMS等待TCP套接字");
  }
}

grpc::Status WriteExactUntil(int fd, std::span<const std::uint8_t> bytes,
                             Deadline deadline) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto waitStatus = WaitForSocketUntil(fd, POLLOUT, deadline);
    if (!waitStatus.ok()) {
      return waitStatus;
    }
    const auto written = send(fd, bytes.data() + offset, bytes.size() - offset,
                              MSG_NOSIGNAL | MSG_DONTWAIT);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    return written == 0
               ? grpc::Status(grpc::StatusCode::UNAVAILABLE,
                              "IEC61850 MMS TCP发送被对端关闭")
               : SystemError("IEC61850 MMS发送TCP数据");
  }
  return grpc::Status::OK;
}

grpc::Status ReadExactUntil(int fd, std::span<std::uint8_t> bytes,
                            Deadline deadline,
                            std::size_t* bytesRead = nullptr) {
  if (bytesRead != nullptr) {
    *bytesRead = 0;
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto waitStatus = WaitForSocketUntil(fd, POLLIN, deadline);
    if (!waitStatus.ok()) {
      return waitStatus;
    }
    const auto received = recv(fd, bytes.data() + offset,
                               bytes.size() - offset, MSG_DONTWAIT);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      if (bytesRead != nullptr) {
        *bytesRead = offset;
      }
      continue;
    }
    if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    return received == 0
               ? grpc::Status(grpc::StatusCode::UNAVAILABLE,
                              "IEC61850 MMS TCP已被对端关闭")
               : SystemError("IEC61850 MMS读取TCP数据");
  }
  return grpc::Status::OK;
}

grpc::Status ReadTpktFrameUntil(int fd, Deadline deadline,
                                std::vector<std::uint8_t>* frame,
                                bool* framePartial = nullptr) {
  if (frame == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS接收帧输出参数为空");
  }
  if (framePartial != nullptr) {
    *framePartial = false;
  }
  frame->clear();
  std::array<std::uint8_t, kTpktHeaderSize> header{};
  std::size_t headerBytes = 0;
  auto status = ReadExactUntil(fd, header, deadline, &headerBytes);
  if (!status.ok()) {
    if (framePartial != nullptr) {
      *framePartial = headerBytes != 0;
    }
    return status;
  }
  const auto length = ReadNetworkUint16(header.data() + 2);
  if (length < kMinimumCotpDataFrameSize) {
    return InvalidFrame("TPKT长度小于COTP最小数据帧");
  }
  frame->resize(length);
  std::copy(header.begin(), header.end(), frame->begin());
  std::size_t bodyBytes = 0;
  status = ReadExactUntil(
      fd,
      std::span<std::uint8_t>(frame->data() + header.size(),
                              frame->size() - header.size()),
      deadline, &bodyBytes);
  if (!status.ok()) {
    if (framePartial != nullptr) {
      *framePartial = headerBytes != 0 || bodyBytes != 0;
    }
    frame->clear();
    return status;
  }
  return grpc::Status::OK;
}

grpc::Status ParseIpv4(std::string_view text, in_addr* address,
                       std::string_view field) {
  if (address == nullptr || text.empty() ||
      inet_pton(AF_INET, std::string(text).c_str(), address) != 1) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        std::format("IEC61850 MMS{}必须是有效IPv4地址", field));
  }
  return grpc::Status::OK;
}

}  // namespace

grpc::Status EncodeCotpConnectionRequest(std::span<std::uint8_t> output,
                                          std::size_t* outputSize,
                                          std::uint16_t sourceReference) {
  if (outputSize == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS连接请求输出长度参数为空");
  }
  *outputSize = 0;
  if (sourceReference == 0 || output.size() < kCotpConnectionRequestSize) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "IEC61850 MMS连接请求缓冲不足或源引用无效");
  }
  constexpr std::array<std::uint8_t, kCotpConnectionRequestSize> request{
      0x03, 0x00, 0x00, 0x16, 0x11, 0xe0, 0x00, 0x00,
      0x00, 0x00, 0x00, 0xc0, 0x01, 0x0a, 0xc1, 0x02,
      0x00, 0x01, 0xc2, 0x02, 0x00, 0x01};
  std::copy(request.begin(), request.end(), output.begin());
  WriteNetworkUint16(output.data() + 8, sourceReference);
  *outputSize = request.size();
  return grpc::Status::OK;
}

grpc::Status ValidateCotpConnectionConfirm(
    std::span<const std::uint8_t> frame) {
  if (frame.size() < 7) {
    return InvalidFrame("COTP连接确认长度不足");
  }
  if (frame[0] != 0x03 || frame[1] != 0x00 ||
      ReadNetworkUint16(frame.data() + 2) != frame.size()) {
    return InvalidFrame("COTP连接确认TPKT头无效");
  }
  const auto length = static_cast<std::size_t>(frame[4]);
  if (length < 2 || 5 + length > frame.size() || frame[5] != 0xd0) {
    return InvalidFrame("COTP连接确认PDU类型或长度无效");
  }
  return grpc::Status::OK;
}

grpc::Status EncodeCotpData(std::span<const std::uint8_t> payload,
                            std::span<std::uint8_t> output,
                            std::size_t* outputSize) {
  if (outputSize == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS数据帧输出长度参数为空");
  }
  *outputSize = 0;
  if (payload.empty() || payload.size() > kCotpSegmentPayloadSize ||
      output.size() < payload.size() + 7) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "IEC61850 MMS数据帧超过TPKT或输出缓冲边界");
  }
  output[0] = 0x03;
  output[1] = 0x00;
  WriteNetworkUint16(output.data() + 2,
                     static_cast<std::uint16_t>(payload.size() + 7));
  output[4] = 0x02;
  output[5] = 0xf0;
  output[6] = 0x80;
  std::copy(payload.begin(), payload.end(), output.begin() + 7);
  *outputSize = payload.size() + 7;
  return grpc::Status::OK;
}

grpc::Status EncodeCotpDataSegments(
    std::span<const std::uint8_t> payload,
    std::vector<std::vector<std::uint8_t>>* frames) {
  if (frames == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS COTP分段输出参数为空");
  }
  frames->clear();
  if (payload.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS COTP分段载荷不能为空");
  }
  if (payload.size() > kMaximumReassembledPayloadSize) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "IEC61850 MMS COTP重组载荷超过下位机上限");
  }
  const auto segmentCount =
      (payload.size() + kCotpSegmentPayloadSize - 1) /
      kCotpSegmentPayloadSize;
  frames->reserve(segmentCount);
  std::size_t offset = 0;
  while (offset < payload.size()) {
    const auto segmentSize =
        std::min(kCotpSegmentPayloadSize, payload.size() - offset);
    auto& frame = frames->emplace_back(segmentSize + 7, 0);
    const bool endOfTransport = offset + segmentSize == payload.size();
    frame[0] = 0x03;
    frame[1] = 0x00;
    WriteNetworkUint16(frame.data() + 2,
                       static_cast<std::uint16_t>(frame.size()));
    frame[4] = 0x02;
    frame[5] = 0xf0;
    frame[6] = static_cast<std::uint8_t>(endOfTransport ? 0x80 : 0x00);
    std::copy_n(payload.begin() + offset, segmentSize, frame.begin() + 7);
    offset += segmentSize;
  }
  return grpc::Status::OK;
}

grpc::Status DecodeCotpData(std::span<const std::uint8_t> frame,
                            std::span<std::uint8_t> payload,
                            std::size_t* payloadSize) {
  bool endOfTransport = false;
  auto status = DecodeCotpDataFrame(frame, payload, payloadSize,
                                    &endOfTransport);
  if (!status.ok()) {
    return status;
  }
  if (!endOfTransport) {
    if (payloadSize != nullptr) {
      *payloadSize = 0;
    }
    return InvalidFrame("COTP数据段缺少EOT结束标志");
  }
  return grpc::Status::OK;
}

grpc::Status DecodeCotpDataFrame(std::span<const std::uint8_t> frame,
                                 std::span<std::uint8_t> payload,
                                 std::size_t* payloadSize,
                                 bool* endOfTransport) {
  if (payloadSize == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS数据帧输出长度参数为空");
  }
  if (endOfTransport == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS COTP EOT输出参数为空");
  }
  *payloadSize = 0;
  *endOfTransport = false;
  const auto headerStatus = ValidateTpktHeader(frame);
  if (!headerStatus.ok()) {
    return headerStatus;
  }
  if (frame.size() < kMinimumCotpDataFrameSize || frame[4] != 0x02 ||
      frame[5] != 0xf0 || (frame[6] & 0x7f) != 0) {
    return InvalidFrame("COTP不是标准数据TPDU");
  }
  const auto payloadSizeValue = frame.size() - kMinimumCotpDataFrameSize;
  if (payloadSizeValue == 0 || payloadSizeValue > payload.size()) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "IEC61850 MMS载荷超过输出缓冲或为空");
  }
  std::copy(frame.begin() + kMinimumCotpDataFrameSize, frame.end(),
            payload.begin());
  *payloadSize = payloadSizeValue;
  *endOfTransport = (frame[6] & 0x80) != 0;
  return grpc::Status::OK;
}

MmsTcpTransport::~MmsTcpTransport() { Close(); }

grpc::Status MmsTcpTransport::Connect(const MmsTransportEndpoint& endpoint,
                                      std::uint32_t timeoutMs) {
  Close();
  if (endpoint.remoteIp.empty() || endpoint.remotePort == 0 ||
      endpoint.connectTimeoutMs == 0 || endpoint.ioTimeoutMs == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS通道地址或超时参数无效");
  }
  if (endpoint.interfaceName.size() >= IFNAMSIZ) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS绑定网卡名称过长");
  }
  const auto effectiveTimeout =
      timeoutMs == 0 ? DefaultConnectBudget(endpoint) : timeoutMs;
  if (effectiveTimeout == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS完整建链超时参数无效");
  }
  const auto startedAt = SteadyClock::now();
  const auto transportBudget = std::min<std::uint32_t>(
      effectiveTimeout, DefaultConnectBudget(endpoint));
  const auto transportDeadline =
      startedAt + std::chrono::milliseconds(transportBudget);
  const auto tcpBudget = std::min(effectiveTimeout, endpoint.connectTimeoutMs);
  const auto tcpDeadline =
      startedAt + std::chrono::milliseconds(tcpBudget);

  in_addr remoteAddress{};
  auto status = ParseIpv4(endpoint.remoteIp, &remoteAddress, "远端地址");
  if (!status.ok()) {
    return status;
  }
  in_addr localAddress{};
  if (!endpoint.localIp.empty()) {
    status = ParseIpv4(endpoint.localIp, &localAddress, "本地地址");
    if (!status.ok()) {
      return status;
    }
  }

  fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) {
    return SystemError("IEC61850 MMS创建TCP套接字");
  }
  const int flags = fcntl(fd_, F_GETFL, 0);
  if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    status = SystemError("IEC61850 MMS设置TCP非阻塞");
    Close();
    return status;
  }
  if (!endpoint.interfaceName.empty()) {
    if (if_nametoindex(endpoint.interfaceName.c_str()) == 0) {
      status = SystemError("IEC61850 MMS获取绑定网卡索引");
      Close();
      return status;
    }
    if (setsockopt(fd_, SOL_SOCKET, SO_BINDTODEVICE,
                   endpoint.interfaceName.c_str(),
                   endpoint.interfaceName.size() + 1) < 0) {
      status = SystemError("IEC61850 MMS绑定网卡", errno == EPERM || errno == EACCES
                                               ? grpc::StatusCode::PERMISSION_DENIED
                                               : grpc::StatusCode::UNAVAILABLE);
      Close();
      return status;
    }
  }
  if (!endpoint.localIp.empty()) {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(0);
    local.sin_addr = localAddress;
    if (bind(fd_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0) {
      status = SystemError("IEC61850 MMS绑定本地地址");
      Close();
      return status;
    }
  }

  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(endpoint.remotePort);
  remote.sin_addr = remoteAddress;
  const auto connectResult =
      connect(fd_, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
  if (connectResult < 0 && errno != EINPROGRESS) {
    status = SystemError("IEC61850 MMS连接远端");
    Close();
    return status;
  }
  if (connectResult < 0) {
    status = WaitForSocketUntil(fd_, POLLOUT, tcpDeadline);
    if (!status.ok()) {
      Close();
      return status;
    }
    int socketError = 0;
    socklen_t socketErrorLength = sizeof(socketError);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socketError,
                   &socketErrorLength) < 0) {
      status = SystemError("IEC61850 MMS读取TCP连接结果");
      Close();
      return status;
    }
    if (socketError != 0) {
      errno = socketError;
      status = SystemError("IEC61850 MMS连接远端");
      Close();
      return status;
    }
  }

  std::array<std::uint8_t, kCotpConnectionRequestSize> request{};
  std::size_t requestSize = 0;
  status = EncodeCotpConnectionRequest(request, &requestSize);
  if (!status.ok()) {
    Close();
    return status;
  }
  LOG_DEBUG("IEC61850 MMS发送COTP连接请求: {}",
            HexDump(std::span<const std::uint8_t>(request.data(), requestSize)));
  status = WriteExactUntil(
      fd_, std::span<const std::uint8_t>(request.data(), requestSize),
      transportDeadline);
  if (!status.ok()) {
    Close();
    return status;
  }
  std::vector<std::uint8_t> confirm;
  status = ReadTpktFrameUntil(fd_, transportDeadline, &confirm);
  if (!status.ok()) {
    Close();
    return status;
  }
  LOG_DEBUG("IEC61850 MMS接收COTP连接确认: {}", HexDump(confirm));
  status = ValidateCotpConnectionConfirm(confirm);
  if (!status.ok()) {
    Close();
    return status;
  }
  ioTimeoutMs_ = endpoint.ioTimeoutMs;
  interfaceName_ = endpoint.interfaceName;
  LOG_INFO("IEC61850 MMS ISO-on-TCP通道已建立: 远端={}:{}, 网卡={}",
           endpoint.remoteIp, endpoint.remotePort,
           endpoint.interfaceName.empty() ? "系统路由" : endpoint.interfaceName);
  return grpc::Status::OK;
}

grpc::Status MmsTcpTransport::Send(std::span<const std::uint8_t> payload) {
  return Send(payload, ioTimeoutMs_);
}

grpc::Status MmsTcpTransport::Send(std::span<const std::uint8_t> payload,
                                   std::uint32_t timeoutMs) {
  if (!IsConnected()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS TCP通道尚未建立");
  }
  const auto effectiveTimeout = timeoutMs == 0 ? ioTimeoutMs_ : timeoutMs;
  if (effectiveTimeout == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS TCP发送超时参数无效");
  }
  std::vector<std::vector<std::uint8_t>> frames;
  auto status = EncodeCotpDataSegments(payload, &frames);
  if (!status.ok()) {
    return status;
  }
  const auto deadline =
      SteadyClock::now() + std::chrono::milliseconds(effectiveTimeout);
  for (const auto& frame : frames) {
    LOG_DEBUG("IEC61850 MMS发送COTP数据段: {}", HexDump(frame));
    status = WriteExactUntil(fd_, frame, deadline);
    if (!status.ok()) {
      if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED &&
          frames.size() > 1) {
        LOG_WARNING("IEC61850 MMS分段发送共享截止时间已耗尽: 分段数={}",
                    frames.size());
      }
      LOG_WARNING("IEC61850 MMS发送COTP数据段失败: {}",
                  status.error_message());
      return status;
    }
  }
  return grpc::Status::OK;
}

grpc::Status MmsTcpTransport::Receive(std::vector<std::uint8_t>* payload,
                                      std::uint32_t timeoutMs) {
  if (payload == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850 MMS接收载荷输出参数为空");
  }
  payload->clear();
  if (!IsConnected()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 MMS TCP通道尚未建立");
  }
  const auto effectiveTimeout = timeoutMs == 0 ? ioTimeoutMs_ : timeoutMs;
  const auto deadline =
      SteadyClock::now() + std::chrono::milliseconds(effectiveTimeout);
  for (;;) {
    std::vector<std::uint8_t> frame;
    bool framePartial = false;
    auto status = ReadTpktFrameUntil(fd_, deadline, &frame, &framePartial);
    if (!status.ok()) {
      const auto receivedBytes = receivePayload_.size();
      if (framePartial) {
        LOG_WARNING("IEC61850 MMS COTP分段未在截止时间内收完整，关闭当前通道: 已接收字节={}",
                    receivedBytes);
        receivePayload_.clear();
        Close();
        if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED) {
          return status;
        }
        return grpc::Status(
            grpc::StatusCode::UNAVAILABLE,
            "IEC61850 MMS COTP分段未完整接收，当前通道已关闭并需要重连");
      }
      if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED &&
          receivedBytes != 0) {
        LOG_WARNING("IEC61850 MMS分段接收共享截止时间已耗尽: 已接收字节={}",
                    receivedBytes);
      }
      if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED) {
        LOG_WARNING("IEC61850 MMS接收COTP数据失败: {}",
                    status.error_message());
        receivePayload_.clear();
        Close();
      }
      return status;
    }
    LOG_DEBUG("IEC61850 MMS接收COTP数据段: {}", HexDump(frame));
    std::vector<std::uint8_t> segmentPayload(frame.size());
    std::size_t segmentSize = 0;
    bool endOfTransport = false;
    status = DecodeCotpDataFrame(frame, segmentPayload, &segmentSize,
                                 &endOfTransport);
    if (!status.ok()) {
      receivePayload_.clear();
      LOG_WARNING("IEC61850 MMS COTP数据段校验失败，关闭当前通道: {}",
                  status.error_message());
      Close();
      return status;
    }
    if (segmentSize > kMaximumReassembledPayloadSize -
                          receivePayload_.size()) {
      receivePayload_.clear();
      LOG_WARNING("IEC61850 MMS重组载荷超过限制，关闭当前通道");
      Close();
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "IEC61850 MMS重组载荷超过下位机上限");
    }
    receivePayload_.insert(receivePayload_.end(), segmentPayload.begin(),
                           segmentPayload.begin() + segmentSize);
    if (endOfTransport) {
      payload->swap(receivePayload_);
      receivePayload_.clear();
      return grpc::Status::OK;
    }
  }
}

void MmsTcpTransport::Close() noexcept {
  if (fd_ >= 0) {
    shutdown(fd_, SHUT_RDWR);
    close(fd_);
  }
  fd_ = -1;
  ioTimeoutMs_ = 1000;
  interfaceName_.clear();
  receivePayload_.clear();
}

bool MmsTcpTransport::IsConnected() const noexcept { return fd_ >= 0; }

}  // namespace IEC61850
