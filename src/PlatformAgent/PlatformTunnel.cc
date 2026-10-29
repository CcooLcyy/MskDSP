#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <thread>

#include "Logger.h"
#include "PlatformAgentInternal.h"
#include "ThreadUtil.hpp"

namespace mskdsp::platformagent {
using namespace std::chrono_literals;

void TunnelConnection::SetCloseReason(const char *reason) {
  std::lock_guard lock(diagnosticMutex);
  if (closeReason.empty())
    closeReason = reason;
}

std::string TunnelConnection::CloseReason() {
  std::lock_guard lock(diagnosticMutex);
  return closeReason.empty() ? "隧道已结束" : closeReason;
}

void TunnelConnection::Cancel(const char *reason) {
  SetCloseReason(reason);
  cancelled = true;
  context.TryCancel();
  Shutdown(SHUT_RDWR);
  event.notify_all();
}

void TunnelConnection::Shutdown(int direction) {
  std::lock_guard lock(socketMutex);
  if (descriptor >= 0)
    shutdown(descriptor, direction);
}

void TunnelConnection::CloseSocket() {
  std::lock_guard lock(socketMutex);
  if (descriptor >= 0)
    close(descriptor);
  descriptor = -1;
}

namespace {
bool WaitSocket(int descriptor, short events, const std::shared_ptr<TunnelConnection> &state, int64_t deadline) {
  while (!state->cancelled.load()) {
    if (deadline != 0 && SteadyMilliseconds() >= deadline)
      return false;
    pollfd item{descriptor, events, 0};
    const int result = poll(&item, 1, 100);
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0)
      return false;
    if (result == 0)
      continue;
    if (item.revents & POLLNVAL)
      return false;
    // 读方向同时可能收到 POLLHUP，先 recv 取完内核中已到达的尾部。
    if ((item.revents & events) || (events == POLLIN && (item.revents & POLLHUP)))
      return true;
    if (item.revents & (POLLERR | POLLHUP))
      return false;
  }
  return false;
}

bool ConnectEndpoint(const ServiceEndpoint &target, const std::shared_ptr<TunnelConnection> &state) {
  const int descriptor = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (descriptor < 0)
    return false;
  {
    std::lock_guard lock(state->socketMutex);
    state->descriptor = descriptor;
  }
  if (state->cancelled.load())
    return false;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  if (inet_pton(AF_INET, target.host.c_str(), &address.sin_addr) != 1)
    return false;
  address.sin_port = htons(target.port);
  if (connect(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
    return true;
  if (errno != EINPROGRESS || !WaitSocket(descriptor, POLLOUT, state, SteadyMilliseconds() + 5000))
    return false;
  int error = 0;
  socklen_t length = sizeof(error);
  return getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
}

bool WriteSocket(const std::string &data, const std::shared_ptr<TunnelConnection> &state) {
  const auto deadline = SteadyMilliseconds() + 30000;
  size_t offset = 0;
  while (offset < data.size() && !state->cancelled.load()) {
    if (!WaitSocket(state->descriptor, POLLOUT, state, deadline))
      return false;
    const auto count = send(state->descriptor, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (count <= 0)
      return false;
    offset += static_cast<size_t>(count);
    state->localWrittenBytes.fetch_add(static_cast<uint64_t>(count));
    state->lastLocalWrite = SteadyMilliseconds();
  }
  return offset == data.size();
}

void ReportFailure(const std::shared_ptr<ControlConnection> &control, const remote::TunnelRequest &request, const char *reason) {
  remote::DeviceMessage message;
  message.mutable_tunnel_failure()->set_tunnel_id(request.tunnel_id());
  message.mutable_tunnel_failure()->set_message(reason);
  LOG_WARNING("平台接入发送隧道失败通知，隧道={}，原因={}", request.tunnel_id(), reason);
  control->Send(message);
}
}  // namespace

void RunTunnel(const Config &config, remote::DeviceAccessService::Stub *stub, remote::TunnelRequest request, const std::shared_ptr<ControlConnection> &control, const std::shared_ptr<TunnelConnection> &state) {
  struct SocketCleanup {
    std::shared_ptr<TunnelConnection> state;
    const Config &config;
    const remote::TunnelRequest &request;
    const int64_t started{SteadyMilliseconds()};
    ~SocketCleanup() {
      state->CloseSocket();
      const auto now = SteadyMilliseconds();
      const auto local = state->lastLocalWrite.load();
      const auto platform = state->lastPlatformWrite.load();
      LOG_INFO("平台隧道传输结束，设备={}，隧道={}，服务={}，平台接收字节={}，本机写入字节={}，本机读取字节={}，平台发送字节={}，持续毫秒={}，本机写入停顿毫秒={}，平台发送停顿毫秒={}，原因={}", config.deviceId, request.tunnel_id(), request.service(), state->receivedBytes.load(), state->localWrittenBytes.load(), state->localReadBytes.load(), state->sentBytes.load(), now - started, local == 0 ? -1 : now - local, platform == 0 ? -1 : now - platform, state->CloseReason());
    }
  } cleanup{state, config, request};
  // 固定服务不依赖管理器查询；普通模块才需要动态发现运行端口。
  const auto running = request.service() == "ModuleManager" || request.service() == "ssh" || request.service() == kVerticalSecurityService
      ? std::vector<RunningModule>{}
      : DiscoverModules(config);
  const auto target = ResolveEndpoint(config, request.service(), running);
  if (!target) {
    state->SetCloseReason("服务未开放或模块当前未运行");
    ReportFailure(control, request, "服务未开放或模块当前未运行");
    return;
  }
  LOG_INFO("平台隧道开始连接受控目标，设备={}，隧道={}，服务={}，目标={}:{}", config.deviceId, request.tunnel_id(), request.service(), target->host, target->port);
  if (!ConnectEndpoint(*target, state)) {
    const char *reason = request.service() == kVerticalSecurityService ? "连接纵密管理服务失败或超时" : "连接本机服务失败或超时";
    state->SetCloseReason(reason);
    ReportFailure(control, request, reason);
    return;
  }
  LOG_INFO("平台隧道受控目标已连接，设备={}，隧道={}，服务={}，目标={}:{}", config.deviceId, request.tunnel_id(), request.service(), target->host, target->port);
  if (!control->active.load() || state->cancelled.load())
    return;
  state->context.AddMetadata("authorization", "Bearer " + config.token);
  state->rpcDeadline = SteadyMilliseconds() + 10000;
  auto watchdog = ModuleManager::StartModuleThread("PlatformAgent", [&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      const auto deadline = state->rpcDeadline.load();
      if (!control->active.load())
        state->Cancel("平台控制连接已关闭");
      else if (deadline != 0 && SteadyMilliseconds() >= deadline)
        state->Cancel("平台隧道 RPC 握手或发送超时");
      std::this_thread::sleep_for(100ms);
    }
  });
  auto stream = stub->AttachTunnel(&state->context);
  auto send = [&](const remote::TunnelFrame &frame) {
    if (state->cancelled.load())
      return false;
    state->rpcDeadline = SteadyMilliseconds() + 30000;
    const bool success = stream->Write(frame);
    state->rpcDeadline = 0;
    if (!success)
      state->Cancel("平台隧道 RPC 发送失败");
    else if (frame.has_data()) {
      state->sentBytes.fetch_add(frame.data().size());
      state->lastPlatformWrite = SteadyMilliseconds();
    }
    return success;
  };
  remote::TunnelFrame frame;
  frame.mutable_attach()->set_tunnel_id(request.tunnel_id());
  frame.mutable_attach()->set_attach_token(request.attach_token());
  if (!send(frame))
    return;
  state->rpcDeadline = SteadyMilliseconds() + 10000;
  const bool ready = stream->Read(&frame) && frame.has_ready();
  state->rpcDeadline = 0;
  if (!ready) {
    state->Cancel("平台隧道配对失败或超时");
    ReportFailure(control, request, "平台隧道配对失败或超时");
    return;
  }
  LOG_INFO("平台隧道已建立，隧道={}，服务={}", request.tunnel_id(), request.service());
  auto receiver = ModuleManager::StartModuleThread("PlatformAgent", [&] {
    remote::TunnelFrame incoming;
    while (!state->cancelled.load() && stream->Read(&incoming)) {
      if (incoming.has_data()) {
        if (state->remoteHalfClosed.load() || incoming.data().empty() || incoming.data().size() > 65536) {
          state->Cancel("平台隧道接收数据无效");
          break;
        }
        state->receivedBytes.fetch_add(incoming.data().size());
        if (!WriteSocket(incoming.data(), state)) {
          LOG_WARNING("平台隧道本机写入失败或超时，隧道={}，平台接收字节={}，本机写入字节={}", request.tunnel_id(), state->receivedBytes.load(), state->localWrittenBytes.load());
          state->Cancel("平台隧道本机写入失败或超时");
          break;
        }
        LOG_DEBUG("平台隧道接收数据，隧道={}，字节数={}", request.tunnel_id(), incoming.data().size());
      } else if (incoming.has_half_close()) {
        if (state->remoteHalfClosed.exchange(true)) {
          state->Cancel("平台隧道重复半关闭");
          break;
        }
        state->Shutdown(SHUT_WR);
        state->event.notify_all();
        LOG_DEBUG("平台隧道收到发送方向半关闭，隧道={}", request.tunnel_id());
      } else if (incoming.has_close()) {
        state->SetCloseReason("平台已关闭隧道");
        state->remoteEnded = true;
        state->Shutdown(SHUT_RDWR);
        state->event.notify_all();
        return;
      } else {
        state->Cancel("平台隧道消息类型无效");
        break;
      }
    }
    // 未收到正常关闭即结束 RPC，属于异常断线，不等待或重放另一方向。
    state->remoteEnded = true;
    state->Cancel("平台隧道接收流异常结束");
  });
  struct ReceiverCleanup {
    std::shared_ptr<TunnelConnection> state;
    std::jthread &receiver;
    ~ReceiverCleanup() {
      if (receiver.joinable()) {
        state->Cancel();
        receiver.join();
      }
    }
  } receiverCleanup{state, receiver};
  std::array<char, 65536> buffer{};
  bool localHalfClosed = false;
  while (!state->cancelled.load() && !state->remoteEnded.load()) {
    if (!WaitSocket(state->descriptor, POLLIN, state, 0)) {
      state->Cancel("等待本机数据失败或连接已取消");
      break;
    }
    const auto count = recv(state->descriptor, buffer.data(), buffer.size(), 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (count < 0) {
      state->Cancel("读取本机数据失败");
      break;
    }
    if (count > 0)
      state->localReadBytes.fetch_add(static_cast<uint64_t>(count));
    if (state->remoteEnded.load() || state->cancelled.load())
      break;
    frame.Clear();
    if (count == 0) {
      frame.mutable_half_close();
      localHalfClosed = send(frame);
      LOG_DEBUG("平台隧道发送方向已半关闭，隧道={}", request.tunnel_id());
      break;
    }
    frame.set_data(buffer.data(), static_cast<size_t>(count));
    if (!send(frame))
      break;
    LOG_DEBUG("平台隧道发送数据，隧道={}，字节数={}", request.tunnel_id(), count);
  }
  if (localHalfClosed && !state->cancelled.load()) {
    std::unique_lock lock(state->eventMutex);
    while (!state->remoteHalfClosed.load() && !state->remoteEnded.load() && !state->cancelled.load())
      state->event.wait_for(lock, 100ms);
  }
  if (localHalfClosed && state->remoteHalfClosed.load() && !state->remoteEnded.load() && !state->cancelled.load()) {
    state->SetCloseReason("双向传输已正常半关闭");
    frame.Clear();
    frame.mutable_close();
    send(frame);
  }
  state->rpcDeadline = SteadyMilliseconds() + 30000;
  stream->WritesDone();
  receiver.join();
  const auto status = stream->Finish();
  state->rpcDeadline = 0;
  watchdog.request_stop();
  watchdog.join();
  LOG_INFO("平台隧道已关闭，隧道={}，状态码={}", request.tunnel_id(), static_cast<int>(status.error_code()));
}
}  // namespace mskdsp::platformagent
