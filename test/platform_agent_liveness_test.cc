#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Logger.h"
#include "ModuleManager.grpc.pb.h"
#include "PlatformAgent.h"
#include "platform/v1/remote.grpc.pb.h"

namespace agent = mskdsp::platformagent;
namespace remote = mskdsp::platform::remote::v1;
using namespace std::chrono_literals;

namespace {
class LivenessPlatform final : public remote::DeviceAccessService::Service {
public:
  bool acknowledge{true};

  grpc::Status ConnectDevice(grpc::ServerContext *context,
                             grpc::ServerReaderWriter<remote::PlatformMessage, remote::DeviceMessage> *stream) override {
    remote::DeviceMessage input;
    if (!stream->Read(&input) || !input.has_hello())
      return {grpc::StatusCode::INVALID_ARGUMENT, "缺少设备握手"};
    {
      std::lock_guard lock(mutex_);
      context_ = context;
    }
    remote::PlatformMessage welcome;
    welcome.mutable_welcome()->set_connection_id("liveness-test");
    welcome.mutable_welcome()->set_heartbeat_interval_seconds(30);
    stream->Write(welcome);
    while (stream->Read(&input)) {
      if (!input.has_heartbeat())
        continue;
      {
        std::lock_guard lock(mutex_);
        ++heartbeats_;
        event_.notify_all();
      }
      if (acknowledge) {
        remote::PlatformMessage ack;
        ack.mutable_heartbeat_ack();
        stream->Write(ack);
      }
    }
    {
      std::lock_guard lock(mutex_);
      context_ = nullptr;
    }
    return grpc::Status::OK;
  }

  bool WaitHeartbeats(unsigned int count, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return event_.wait_for(lock, timeout, [&] { return heartbeats_ >= count; });
  }

  void Disconnect() {
    std::lock_guard lock(mutex_);
    if (context_)
      context_->TryCancel();
  }

private:
  std::mutex mutex_;
  std::condition_variable event_;
  grpc::ServerContext *context_{nullptr};
  unsigned int heartbeats_{0};
};

class LivenessModuleManager final : public ModuleManagerProto::ModuleManage::Service {
public:
  bool block{false};

  grpc::Status GetRunningModuleInfo(grpc::ServerContext *, const ModuleManagerProto::Empty *,
                                    ModuleManagerProto::ModuleRunningInfos *) override {
    std::unique_lock lock(mutex_);
    entered_ = true;
    event_.notify_all();
    if (block)
      event_.wait_for(lock, 5s, [&] { return released_; });
    return grpc::Status::OK;
  }

  bool WaitEntered() {
    std::unique_lock lock(mutex_);
    return event_.wait_for(lock, 5s, [&] { return entered_; });
  }

  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    event_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable event_;
  bool entered_{false};
  bool released_{false};
};

class PlatformAgentLiveness : public ::testing::Test {
protected:
  void SetUp() override {
    ModuleManager::Logger::ensureModuleSink("PlatformAgent");
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(&platform_);
    builder.RegisterService(&manager_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_), grpc::InsecureChannelCredentials());
    ASSERT_TRUE(channel->WaitForConnected(std::chrono::system_clock::now() + 10s));
  }

  void Start() {
    agent::Config config;
    config.enabled = true;
    config.target = "127.0.0.1:" + std::to_string(port_);
    config.deviceId = "device-liveness";
    config.token = "test-token";
    config.managerPort = port_;
    client_ = std::make_unique<agent::PlatformAgent>(config, [this](bool online, std::string) {
      std::lock_guard lock(mutex_);
      if (online)
        online_ = true;
      else if (online_)
        offline_ = true;
      event_.notify_all();
    });
    worker_ = std::jthread([this](std::stop_token stop) { client_->Run(stop); });
  }

  bool WaitOnline() {
    std::unique_lock lock(mutex_);
    return event_.wait_for(lock, 5s, [&] { return online_; });
  }

  bool WaitOffline(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return event_.wait_for(lock, timeout, [&] { return offline_; });
  }

  void TearDown() override {
    manager_.Release();
    if (worker_.joinable()) {
      worker_.request_stop();
      worker_.join();
    }
    if (server_)
      server_->Shutdown(std::chrono::system_clock::now() + 2s);
  }

  LivenessPlatform platform_;
  LivenessModuleManager manager_;

private:
  int port_{0};
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<agent::PlatformAgent> client_;
  std::jthread worker_;
  std::mutex mutex_;
  std::condition_variable event_;
  bool online_{false};
  bool offline_{false};
};

// 验证旧平台下发 30 秒间隔时，立即心跳之后仍在 5 秒周期发送下一次心跳。
TEST_F(PlatformAgentLiveness, CapsLegacyWelcomeHeartbeatAtFiveSeconds) {
  Start();
  ASSERT_TRUE(WaitOnline());
  ASSERT_TRUE(platform_.WaitHeartbeats(1, 5s));
  EXPECT_TRUE(platform_.WaitHeartbeats(2, 7s));
}

// 验证平台持续接收设备心跳却不回复时，从最后有效平台消息起约 20 秒报告离线。
TEST_F(PlatformAgentLiveness, ReportsOfflineAfterTwentySecondsWithoutPlatformMessages) {
  platform_.acknowledge = false;
  Start();
  ASSERT_TRUE(WaitOnline());
  EXPECT_FALSE(WaitOffline(17s));
  EXPECT_TRUE(WaitOffline(7s));
}

// 验证控制流断开时先通知离线，即使心跳线程仍阻塞在本机模块查询中也不等待清理。
TEST_F(PlatformAgentLiveness, ReportsOfflineBeforeWaitingForHeartbeatCleanup) {
  manager_.block = true;
  Start();
  ASSERT_TRUE(WaitOnline());
  ASSERT_TRUE(manager_.WaitEntered());
  platform_.Disconnect();
  EXPECT_TRUE(WaitOffline(1s));
  manager_.Release();
}
}  // namespace
