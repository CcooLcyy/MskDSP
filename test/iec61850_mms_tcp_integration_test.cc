#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <format>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "IEC61850MmsBer.h"
#include "IEC61850MmsPdu.h"
#include "IEC61850MmsService.h"
#include "IEC61850MmsIsoSession.h"
#include "IEC61850MmsTransport.h"
#include "IEC61850MmsWorker.h"

namespace {

using namespace std::chrono_literals;

void AppendTlv(std::vector<std::uint8_t>* output, std::uint8_t tag,
               std::span<const std::uint8_t> value) {
  ASSERT_NE(output, nullptr);
  ASSERT_LE(value.size(), 0xffffu);
  output->push_back(tag);
  if (value.size() <= 127u) {
    output->push_back(static_cast<std::uint8_t>(value.size()));
  } else if (value.size() <= 0xffu) {
    output->push_back(0x81);
    output->push_back(static_cast<std::uint8_t>(value.size()));
  } else {
    output->push_back(0x82);
    output->push_back(static_cast<std::uint8_t>(value.size() >> 8));
    output->push_back(static_cast<std::uint8_t>(value.size()));
  }
  output->insert(output->end(), value.begin(), value.end());
}

void AppendTlv(std::vector<std::uint8_t>* output, std::uint8_t tag,
               std::initializer_list<std::uint8_t> value) {
  AppendTlv(output, tag, std::span<const std::uint8_t>(value.begin(),
                                                       value.size()));
}

bool WriteAll(int fd, std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto written = send(fd, bytes.data() + offset, bytes.size() - offset,
                              MSG_NOSIGNAL);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

bool ReadExact(int fd, std::span<std::uint8_t> bytes,
               std::chrono::milliseconds timeout) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
    const auto timeoutMs = static_cast<int>(std::min<std::int64_t>(
        timeout.count(), std::numeric_limits<int>::max()));
    const auto ready = poll(&descriptor, 1, timeoutMs);
    if (ready <= 0) {
      return false;
    }
    const auto received = recv(fd, bytes.data() + offset, bytes.size() - offset,
                               0);
    if (received <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(received);
  }
  return true;
}

bool ReadTpktFrame(int fd, std::vector<std::uint8_t>* frame,
                   std::chrono::milliseconds timeout = 2s) {
  if (frame == nullptr) {
    return false;
  }
  std::array<std::uint8_t, 4> header{};
  if (!ReadExact(fd, header, timeout) || header[0] != 0x03 ||
      header[1] != 0x00) {
    return false;
  }
  const auto length = static_cast<std::size_t>(
      (static_cast<std::uint16_t>(header[2]) << 8) | header[3]);
  if (length < 7u || length > 0xffffu) {
    return false;
  }
  frame->assign(length, 0);
  std::copy(header.begin(), header.end(), frame->begin());
  return ReadExact(fd, std::span<std::uint8_t>(frame->data() + 4, length - 4),
                   timeout);
}

bool ReadCotpPayload(int fd, std::vector<std::uint8_t>* payload) {
  if (payload == nullptr) {
    return false;
  }
  payload->clear();
  for (;;) {
    std::vector<std::uint8_t> frame;
    if (!ReadTpktFrame(fd, &frame)) {
      return false;
    }
    std::array<std::uint8_t, 2048> segment{};
    std::size_t segmentSize = 0;
    bool endOfTransport = false;
    if (!IEC61850::DecodeCotpDataFrame(frame, segment, &segmentSize,
                                       &endOfTransport)
             .ok()) {
      return false;
    }
    payload->insert(payload->end(), segment.begin(),
                    segment.begin() + segmentSize);
    if (endOfTransport) {
      return true;
    }
  }
}

bool SendCotpPayload(int fd, std::span<const std::uint8_t> payload) {
  std::vector<std::vector<std::uint8_t>> frames;
  if (!IEC61850::EncodeCotpDataSegments(payload, &frames).ok()) {
    return false;
  }
  for (const auto& frame : frames) {
    if (!WriteAll(fd, frame)) {
      return false;
    }
  }
  return true;
}

std::vector<std::uint8_t> MakeNameListResponse(std::uint32_t invokeId) {
  std::vector<std::uint8_t> service;
  const std::vector<std::uint8_t> identifiers;
  AppendTlv(&service, 0xa0, identifiers);
  AppendTlv(&service, 0x81, {0x00});

  std::array<std::uint8_t, 4096> mmsBuffer{};
  std::size_t mmsSize = 0;
  if (!IEC61850::EncodeMmsConfirmedResponse(invokeId, 1, service, mmsBuffer,
                                            &mmsSize)
           .ok()) {
    return {};
  }
  std::array<std::uint8_t, 4096> presentationBuffer{};
  std::size_t presentationSize = 0;
  if (!IEC61850::EncodeMmsPresentationData(
           std::span<const std::uint8_t>(mmsBuffer.data(), mmsSize),
           presentationBuffer, &presentationSize)
           .ok()) {
    return {};
  }
  std::array<std::uint8_t, 4096> sessionBuffer{};
  std::size_t sessionSize = 0;
  if (!IEC61850::EncodeIsoSessionData(
           std::span<const std::uint8_t>(presentationBuffer.data(),
                                         presentationSize),
           sessionBuffer, &sessionSize)
           .ok()) {
    return {};
  }
  return {sessionBuffer.begin(), sessionBuffer.begin() + sessionSize};
}

std::vector<std::uint8_t> MakeSessionAccept() {
  IEC61850::MmsInitiateResponse response;
  response.negotiatedParameterSupport.size = 2;
  response.negotiatedParameterSupport.unusedBits = 5;
  response.negotiatedServiceSupport.size = 11;
  response.negotiatedServiceSupport.unusedBits = 3;
  response.negotiatedServiceSupport.bytes[0] = 0x4e;
  response.negotiatedServiceSupport.bytes[1] = 0x08;

  std::array<std::uint8_t, 4096> initiateBuffer{};
  std::size_t initiateSize = 0;
  if (!IEC61850::EncodeMmsInitiateResponse(response, initiateBuffer,
                                           &initiateSize)
           .ok()) {
    return {};
  }
  std::array<std::uint8_t, 4096> aareBuffer{};
  std::size_t aareSize = 0;
  if (!IEC61850::EncodeMmsAare(
           IEC61850::kMmsApplicationContextOid, 0,
           std::span<const std::uint8_t>(initiateBuffer.data(), initiateSize),
           aareBuffer, &aareSize)
           .ok()) {
    return {};
  }
  std::array<std::uint8_t, 4096> sessionBuffer{};
  std::size_t sessionSize = 0;
  if (!IEC61850::EncodeIsoSessionAccept(
           std::span<const std::uint8_t>(aareBuffer.data(), aareSize),
           sessionBuffer, &sessionSize)
           .ok()) {
    return {};
  }
  return {sessionBuffer.begin(), sessionBuffer.begin() + sessionSize};
}

class LocalMmsIedSimulator final {
public:
  explicit LocalMmsIedSimulator(
      std::chrono::milliseconds connectionConfirmDelay =
          std::chrono::milliseconds::zero())
      : connectionConfirmDelay_(connectionConfirmDelay) {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
      throw std::runtime_error(std::format("创建模拟IED监听套接字失败: {}",
                                           std::strerror(errno)));
    }
    int reuse = 1;
    if (setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) != 0) {
      CloseListenSocket();
      throw std::runtime_error(std::format("设置模拟IED套接字失败: {}",
                                           std::strerror(errno)));
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(listenFd_, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0) {
      CloseListenSocket();
      throw std::runtime_error(std::format("绑定模拟IED回环地址失败: {}",
                                           std::strerror(errno)));
    }
    socklen_t addressLength = sizeof(address);
    if (getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address),
                    &addressLength) != 0) {
      CloseListenSocket();
      throw std::runtime_error(std::format("读取模拟IED端口失败: {}",
                                           std::strerror(errno)));
    }
    port_ = ntohs(address.sin_port);
    if (listen(listenFd_, 1) != 0) {
      CloseListenSocket();
      throw std::runtime_error(std::format("启动模拟IED监听失败: {}",
                                           std::strerror(errno)));
    }
    thread_ = std::jthread([this](std::stop_token stopToken) {
      Run(stopToken);
    });
  }

  ~LocalMmsIedSimulator() {
    thread_.request_stop();
    CloseListenSocket();
    const auto client = clientFd_.exchange(-1);
    if (client >= 0) {
      shutdown(client, SHUT_RDWR);
      close(client);
    }
  }

  LocalMmsIedSimulator(const LocalMmsIedSimulator&) = delete;
  LocalMmsIedSimulator& operator=(const LocalMmsIedSimulator&) = delete;

  std::uint16_t port() const noexcept { return port_; }

  std::size_t requestCount() const noexcept {
    return requestCount_.load(std::memory_order_relaxed);
  }

  int stage() const noexcept { return stage_.load(std::memory_order_relaxed); }

private:
  void CloseListenSocket() noexcept {
    if (listenFd_ >= 0) {
      shutdown(listenFd_, SHUT_RDWR);
      close(listenFd_);
      listenFd_ = -1;
    }
  }

  void Run(std::stop_token stopToken) {
    pollfd descriptor{.fd = listenFd_, .events = POLLIN, .revents = 0};
    while (!stopToken.stop_requested()) {
      const auto ready = poll(&descriptor, 1, 100);
      if (ready <= 0) {
        continue;
      }
      const auto client = accept(listenFd_, nullptr, nullptr);
      if (client < 0) {
        continue;
      }
      stage_.store(1, std::memory_order_relaxed);
      clientFd_.store(client, std::memory_order_release);
      HandleClient(client);
      clientFd_.store(-1, std::memory_order_release);
      shutdown(client, SHUT_RDWR);
      close(client);
    }
  }

  void HandleClient(int client) {
    std::vector<std::uint8_t> frame;
    if (!ReadTpktFrame(client, &frame)) {
      return;
    }
    stage_.store(2, std::memory_order_relaxed);
    constexpr std::array<std::uint8_t, 13> confirm{
        0x03, 0x00, 0x00, 0x0d, 0x08, 0xd0, 0x00, 0x01,
        0x00, 0x01, 0x00, 0xc0, 0x01};
    if (connectionConfirmDelay_ > std::chrono::milliseconds::zero()) {
      std::this_thread::sleep_for(connectionConfirmDelay_);
    }
    if (!WriteAll(client, confirm)) {
      return;
    }
    stage_.store(3, std::memory_order_relaxed);

    std::vector<std::uint8_t> connectPayload;
    if (!ReadCotpPayload(client, &connectPayload)) {
      return;
    }
    IEC61850::IsoSessionPduView connectPdu;
    if (!IEC61850::DecodeIsoSessionPdu(connectPayload, &connectPdu).ok() ||
        connectPdu.type != IEC61850::IsoSessionPduType::CONNECT) {
      return;
    }
    stage_.store(4, std::memory_order_relaxed);
    const auto acceptPayload = MakeSessionAccept();
    if (acceptPayload.empty() || !SendCotpPayload(client, acceptPayload)) {
      return;
    }
    stage_.store(5, std::memory_order_relaxed);

    std::vector<std::uint8_t> requestPayload;
    if (!ReadCotpPayload(client, &requestPayload)) {
      return;
    }
    IEC61850::IsoSessionPduView requestSession;
    if (!IEC61850::DecodeIsoSessionPdu(requestPayload, &requestSession).ok() ||
        requestSession.type != IEC61850::IsoSessionPduType::DATA) {
      return;
    }
    std::span<const std::uint8_t> mmsPdu;
    if (!IEC61850::DecodeMmsPresentationData(requestSession.userData, &mmsPdu)
             .ok()) {
      return;
    }
    IEC61850::MmsConfirmedPduView request;
    if (!IEC61850::DecodeMmsConfirmedRequest(mmsPdu, &request).ok() ||
        request.serviceTag != 1) {
      return;
    }
    stage_.store(6, std::memory_order_relaxed);
    requestCount_.fetch_add(1, std::memory_order_relaxed);
    const auto responsePayload = MakeNameListResponse(request.invokeId);
    if (responsePayload.empty()) {
      return;
    }
    SendCotpPayload(client, responsePayload);
    stage_.store(7, std::memory_order_relaxed);

    // 保持会话存活，允许工作器观察到READY后由测试主动停止。
    while (clientFd_.load(std::memory_order_acquire) == client) {
      pollfd descriptor{.fd = client, .events = POLLIN, .revents = 0};
      const auto ready = poll(&descriptor, 1, 100);
      if (ready < 0 || (ready > 0 && (descriptor.revents & POLLHUP) != 0)) {
        break;
      }
    }
  }

  int listenFd_ = -1;
  std::atomic<int> clientFd_{-1};
  std::uint16_t port_ = 0;
  std::atomic<std::size_t> requestCount_{0};
  std::atomic<int> stage_{0};
  std::chrono::milliseconds connectionConfirmDelay_{};
  std::jthread thread_;
};

class LocalCotpProbe final {
public:
  explicit LocalCotpProbe(
      std::vector<std::uint8_t> response,
      std::chrono::milliseconds segmentDelay = std::chrono::milliseconds::zero())
      : response_(std::move(response)), segmentDelay_(segmentDelay) {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
      throw std::runtime_error(std::format("创建COTP探针套接字失败: {}",
                                           std::strerror(errno)));
    }
    int reuse = 1;
    if (setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) != 0) {
      CloseListenSocket();
      throw std::runtime_error(std::format("设置COTP探针套接字失败: {}",
                                           std::strerror(errno)));
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(listenFd_, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0) {
      CloseListenSocket();
      throw std::runtime_error(std::format("绑定COTP探针地址失败: {}",
                                           std::strerror(errno)));
    }
    socklen_t addressLength = sizeof(address);
    if (getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address),
                    &addressLength) != 0 ||
        listen(listenFd_, 1) != 0) {
      CloseListenSocket();
      throw std::runtime_error(std::format("启动COTP探针监听失败: {}",
                                           std::strerror(errno)));
    }
    port_ = ntohs(address.sin_port);
    thread_ = std::jthread([this](std::stop_token stopToken) {
      Run(stopToken);
    });
  }

  ~LocalCotpProbe() {
    thread_.request_stop();
    CloseListenSocket();
    const auto client = clientFd_.exchange(-1);
    if (client >= 0) {
      shutdown(client, SHUT_RDWR);
      close(client);
    }
  }

  LocalCotpProbe(const LocalCotpProbe&) = delete;
  LocalCotpProbe& operator=(const LocalCotpProbe&) = delete;

  std::uint16_t port() const noexcept { return port_; }

  bool WaitForRequest(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this] { return requestReceived_; });
  }

  std::vector<std::uint8_t> request() const {
    std::lock_guard lock(mutex_);
    return request_;
  }

private:
  void CloseListenSocket() noexcept {
    if (listenFd_ >= 0) {
      shutdown(listenFd_, SHUT_RDWR);
      close(listenFd_);
      listenFd_ = -1;
    }
  }

  void Run(std::stop_token stopToken) {
    pollfd descriptor{.fd = listenFd_, .events = POLLIN, .revents = 0};
    while (!stopToken.stop_requested()) {
      if (poll(&descriptor, 1, 100) <= 0) {
        continue;
      }
      const auto client = accept(listenFd_, nullptr, nullptr);
      if (client < 0) {
        continue;
      }
      clientFd_.store(client, std::memory_order_release);
      HandleClient(client);
      clientFd_.store(-1, std::memory_order_release);
      shutdown(client, SHUT_RDWR);
      close(client);
    }
  }

  void HandleClient(int client) {
    std::vector<std::uint8_t> frame;
    if (!ReadTpktFrame(client, &frame)) {
      return;
    }
    constexpr std::array<std::uint8_t, 13> confirm{
        0x03, 0x00, 0x00, 0x0d, 0x08, 0xd0, 0x00, 0x01,
        0x00, 0x01, 0x00, 0xc0, 0x01};
    if (!WriteAll(client, confirm)) {
      return;
    }
    std::vector<std::uint8_t> request;
    if (!ReadCotpPayload(client, &request)) {
      return;
    }
    {
      std::lock_guard lock(mutex_);
      request_ = request;
      requestReceived_ = true;
    }
    condition_.notify_all();
    std::vector<std::vector<std::uint8_t>> responseFrames;
    if (!IEC61850::EncodeCotpDataSegments(response_, &responseFrames).ok()) {
      return;
    }
    for (std::size_t index = 0; index < responseFrames.size(); ++index) {
      if (index != 0 && segmentDelay_ > std::chrono::milliseconds::zero()) {
        std::this_thread::sleep_for(segmentDelay_);
      }
      if (!WriteAll(client, responseFrames[index])) {
        return;
      }
    }
    while (clientFd_.load(std::memory_order_acquire) == client) {
      pollfd waitDescriptor{.fd = client, .events = POLLIN, .revents = 0};
      if (poll(&waitDescriptor, 1, 100) < 0) {
        break;
      }
    }
  }

  int listenFd_ = -1;
  std::atomic<int> clientFd_{-1};
  std::uint16_t port_ = 0;
  std::vector<std::uint8_t> response_;
  std::chrono::milliseconds segmentDelay_{};
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool requestReceived_ = false;
  std::vector<std::uint8_t> request_;
  std::jthread thread_;
};

IEC61850::ProtocolIedPlan MakeMinimalPlan() {
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_conn_name("tcp-ied");
  plan.config.set_ied_name("IED1");
  plan.config.set_access_point("AP1");
  plan.ied.set_name("IED1");
  return plan;
}

IEC61850::ProtocolNetworkBinding MakeBinding(std::uint16_t port) {
  IEC61850::ProtocolNetworkBinding binding;
  binding.channel.set_channel(IEC61850Proto::NETWORK_CHANNEL_A);
  binding.channel.set_enabled(true);
  binding.channel.set_local_ip("127.0.0.1");
  binding.channel.set_remote_ip("127.0.0.1");
  binding.channel.set_remote_port(port);
  return binding;
}

}  // namespace

// 验证生产MmsTcpTransport可以通过真实TCP完成COTP、Session、ACSE、Initiate和NameList闭环。
TEST(IEC61850MmsTcpIntegrationTest, EstablishesReadySessionAgainstLocalIed) {
  LocalMmsIedSimulator simulator;
  std::mutex stateMutex;
  std::condition_variable stateCondition;
  bool ready = false;
  std::string lastError;
  IEC61850::ProtocolEventCallbacks callbacks;
  callbacks.onMmsConnection = [&](IEC61850::MmsConnectionEvent event) {
    std::lock_guard lock(stateMutex);
    if (!event.error.empty()) {
      lastError = event.error;
    }
    if (event.state == IEC61850::ProtocolSessionState::READY) {
      ready = true;
    }
    stateCondition.notify_all();
  };

  auto plan = MakeMinimalPlan();
  IEC61850::MmsSessionWorker worker(plan, {MakeBinding(simulator.port())},
                                    std::move(callbacks));
  ASSERT_TRUE(worker.Start().ok());
  {
    std::unique_lock lock(stateMutex);
    ASSERT_TRUE(stateCondition.wait_for(lock, 3s, [&] { return ready; }))
        << "最近一次MMS状态错误: " << lastError
        << ", 模拟IED阶段=" << simulator.stage();
  }
  EXPECT_EQ(simulator.requestCount(), 1u);
  worker.Stop();
}

// 验证TCP连接建立后的COTP确认阶段继续消费Connect传入的同一总截止时间。
TEST(IEC61850MmsTcpIntegrationTest,
     ConnectUsesOneDeadlineForTcpAndCotpHandshake) {
  LocalMmsIedSimulator simulator(150ms);
  IEC61850::MmsTcpTransport transport;
  IEC61850::MmsTransportEndpoint endpoint;
  endpoint.remoteIp = "127.0.0.1";
  endpoint.remotePort = simulator.port();
  endpoint.connectTimeoutMs = 1000;
  endpoint.ioTimeoutMs = 1000;

  const auto status = transport.Connect(endpoint, 50);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_FALSE(transport.IsConnected());
  transport.Close();
}

// 验证生产MmsTcpTransport在真实TCP上能够完成COTP大载荷分段发送和接收重组。
TEST(IEC61850MmsTcpIntegrationTest, ExchangesSegmentedCotpPayloadOverTcp) {
  std::vector<std::uint8_t> response(2500, 0xa5);
  LocalCotpProbe probe(response);
  IEC61850::MmsTcpTransport transport;
  IEC61850::MmsTransportEndpoint endpoint;
  endpoint.remoteIp = "127.0.0.1";
  endpoint.remotePort = probe.port();
  endpoint.connectTimeoutMs = 1000;
  endpoint.ioTimeoutMs = 1000;
  ASSERT_TRUE(transport.Connect(endpoint).ok());

  const std::vector<std::uint8_t> request(2500, 0x5a);
  ASSERT_TRUE(transport.Send(request).ok());
  ASSERT_TRUE(probe.WaitForRequest(2s));
  EXPECT_EQ(probe.request(), request);

  std::vector<std::uint8_t> received;
  ASSERT_TRUE(transport.Receive(&received, 1000).ok());
  EXPECT_EQ(received, response);
  transport.Close();
}

// 验证COTP多分段接收共享一次调用的绝对截止时间，不因分段到达而重复延长等待窗口。
TEST(IEC61850MmsTcpIntegrationTest,
     ReceiveUsesSingleDeadlineForSegmentedCotpPayload) {
  const std::vector<std::uint8_t> response(2500, 0xa5);
  LocalCotpProbe probe(response, 35ms);
  IEC61850::MmsTcpTransport transport;
  IEC61850::MmsTransportEndpoint endpoint;
  endpoint.remoteIp = "127.0.0.1";
  endpoint.remotePort = probe.port();
  endpoint.connectTimeoutMs = 1000;
  endpoint.ioTimeoutMs = 1000;
  ASSERT_TRUE(transport.Connect(endpoint).ok());

  const std::vector<std::uint8_t> request(1, 0x5a);
  ASSERT_TRUE(transport.Send(request).ok());
  ASSERT_TRUE(probe.WaitForRequest(2s));

  std::vector<std::uint8_t> received;
  const auto status = transport.Receive(&received, 60);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_TRUE(received.empty());
  transport.Close();
}

// 验证分段间隔超过单次轮询窗口时，后续Receive仍能接着已有前缀完成重组。
TEST(IEC61850MmsTcpIntegrationTest,
     ReceivePreservesCotpPrefixAcrossPollingTimeout) {
  const std::vector<std::uint8_t> response(2500, 0xa5);
  LocalCotpProbe probe(response, 80ms);
  IEC61850::MmsTcpTransport transport;
  IEC61850::MmsTransportEndpoint endpoint;
  endpoint.remoteIp = "127.0.0.1";
  endpoint.remotePort = probe.port();
  endpoint.connectTimeoutMs = 1000;
  endpoint.ioTimeoutMs = 1000;
  ASSERT_TRUE(transport.Connect(endpoint).ok());

  const std::vector<std::uint8_t> request(1, 0x5a);
  ASSERT_TRUE(transport.Send(request).ok());
  ASSERT_TRUE(probe.WaitForRequest(2s));

  std::vector<std::uint8_t> firstAttempt;
  const auto firstStatus = transport.Receive(&firstAttempt, 30);
  EXPECT_EQ(firstStatus.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_TRUE(firstAttempt.empty());

  std::vector<std::uint8_t> received;
  ASSERT_TRUE(transport.Receive(&received, 250).ok());
  EXPECT_EQ(received, response);
  transport.Close();
}
