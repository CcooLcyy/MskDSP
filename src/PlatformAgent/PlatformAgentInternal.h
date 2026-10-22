#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "PlatformAgent.h"
#include "platform/v1/remote.grpc.pb.h"

namespace mskdsp::platformagent {
namespace remote = ::mskdsp::platform::remote::v1;

int64_t SteadyMilliseconds();
std::vector<RunningModule> DiscoverModules(const Config &config);

struct ControlConnection {
  grpc::ClientContext context;
  std::unique_ptr<grpc::ClientReaderWriter<remote::DeviceMessage, remote::PlatformMessage>> stream;
  std::mutex writeMutex;
  std::atomic<bool> active{true};
  std::atomic<int64_t> writeDeadline{0};
  std::atomic<int64_t> lastReceived{0};
  std::atomic<bool> welcomed{false};
  std::atomic<unsigned int> heartbeatSeconds{30};
  bool Send(const remote::DeviceMessage &message);
  void Cancel();
};

struct TunnelConnection {
  grpc::ClientContext context;
  std::atomic<bool> cancelled{false};
  std::atomic<bool> completed{false};
  std::atomic<int64_t> rpcDeadline{0};
  std::mutex socketMutex;
  int descriptor{-1};
  std::mutex eventMutex;
  std::condition_variable event;
  std::atomic<bool> remoteHalfClosed{false};
  std::atomic<bool> remoteEnded{false};
  std::atomic<uint64_t> receivedBytes{0};
  std::atomic<uint64_t> localWrittenBytes{0};
  std::atomic<uint64_t> localReadBytes{0};
  std::atomic<uint64_t> sentBytes{0};
  std::atomic<int64_t> lastLocalWrite{0};
  std::atomic<int64_t> lastPlatformWrite{0};
  std::mutex diagnosticMutex;
  std::string closeReason;
  void SetCloseReason(const char *reason);
  std::string CloseReason();
  void Cancel(const char *reason = "连接已取消");
  void Shutdown(int direction);
  void CloseSocket();
};

void RunTunnel(const Config &config, remote::DeviceAccessService::Stub *stub, remote::TunnelRequest request, const std::shared_ptr<ControlConnection> &control, const std::shared_ptr<TunnelConnection> &tunnel);
}  // namespace mskdsp::platformagent
