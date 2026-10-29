#include <arpa/inet.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "Logger.h"
#include "ModuleManager.grpc.pb.h"
#include "PlatformAgent.h"
#include "platform/v1/remote.grpc.pb.h"

namespace agent = mskdsp::platformagent;
namespace remote = mskdsp::platform::remote::v1;
using namespace std::chrono_literals;

// 验证默认禁用，不要求未启用连接功能的设备凭据。
TEST(PlatformAgentConfig, DisabledByDefault) {
  const auto config = agent::LoadConfig([](std::string_view) { return std::string(); });
  EXPECT_FALSE(config.enabled);
  EXPECT_FALSE(config.sshEnabled);
}

// 验证启用时缺少设备身份被拒绝，不把无凭据连接送往平台。
TEST(PlatformAgentConfig, EnabledRequiresIdentity) {
  EXPECT_THROW(agent::LoadConfig([](std::string_view name) {
                 return name == "MSKDSP_PLATFORM_ENABLED" ? "1" : "";
               }),
               std::invalid_argument);
}

// 验证明文平台目标可以使用远端主机，不需要额外协议开关。
TEST(PlatformAgentConfig, PlaintextAllowsRemoteTarget) {
  std::map<std::string, std::string> env{
      {"MSKDSP_PLATFORM_ENABLED", "1"}, {"MSKDSP_PLATFORM_TARGET", "example.org:50051"}, {"MSKDSP_PLATFORM_DEVICE_ID", "device-a"}, {"MSKDSP_PLATFORM_DEVICE_TOKEN", "token"}};
  const auto config = agent::LoadConfig([&](std::string_view name) { return env[std::string(name)]; });
  EXPECT_EQ(config.target, "example.org:50051");
}

// 验证明文选择不会绕过平台地址格式或设备身份校验。
TEST(PlatformAgentConfig, PlaintextStillRequiresValidTargetAndIdentity) {
  std::map<std::string, std::string> env{
      {"MSKDSP_PLATFORM_ENABLED", "1"}, {"MSKDSP_PLATFORM_TARGET", "example.org:0"}, {"MSKDSP_PLATFORM_DEVICE_ID", "device-a"}, {"MSKDSP_PLATFORM_DEVICE_TOKEN", "token"}};
  EXPECT_THROW(agent::LoadConfig([&](std::string_view name) { return env[std::string(name)]; }), std::invalid_argument);
  env["MSKDSP_PLATFORM_TARGET"] = "example.org:50051";
  env["MSKDSP_PLATFORM_DEVICE_TOKEN"] = "";
  EXPECT_THROW(agent::LoadConfig([&](std::string_view name) { return env[std::string(name)]; }), std::invalid_argument);
}

// 验证只接受白名单中运行模块，远端不能把 IP 或端口当成服务名称。
TEST(PlatformAgentTargets, RejectsArbitraryTargetsAndStoppedModules) {
  agent::Config config;
  config.allowedModules = {"DataCenter"};
  std::vector<agent::RunningModule> running{{"DataCenter", "0.0.0.0:17321"}, {"IEC104", "0.0.0.0:17322"}};
  EXPECT_EQ(agent::ResolveTarget(config, "DataCenter", running), 17321);
  EXPECT_FALSE(agent::ResolveTarget(config, "IEC104", running));
  EXPECT_FALSE(agent::ResolveTarget(config, "127.0.0.1:22", running));
  EXPECT_FALSE(agent::ResolveTarget(config, "DataCenter", {}));
  EXPECT_FALSE(agent::ResolveTarget(config, "ssh", running));
  running[0].address = "10.0.0.1:17321";
  EXPECT_FALSE(agent::ResolveTarget(config, "DataCenter", running));
}

// 验证心跳发送完整可用服务集合，卸载模块和禁用 SSH 后不会继续公布旧服务。
TEST(PlatformAgentTargets, AnnouncesOnlyCurrentAllowedServices) {
  agent::Config config;
  config.allowedModules = {"DataCenter"};
  EXPECT_EQ(agent::AvailableServices(config, {{"DataCenter", "0.0.0.0:17321"}}), (std::vector<std::string>{"ModuleManager", "DataCenter"}));
  EXPECT_EQ(agent::AvailableServices(config, {}), (std::vector<std::string>{"ModuleManager"}));
  config.sshEnabled = true;
  EXPECT_EQ(agent::ResolveTarget(config, "ssh", {}), 22);
  EXPECT_EQ(agent::AvailableServices(config, {}), (std::vector<std::string>{"ModuleManager", "ssh", "vertical-security"}));
}

// 验证纵密维护能力由 SSH 维护开关控制，且始终解析到固定管理服务。
TEST(PlatformAgentTargets, VerticalSecurityUsesFixedMaintenanceEndpoint) {
  agent::Config config;
  config.allowedModules = {"vertical-security", "DataCenter"};
  const std::vector<agent::RunningModule> running{{"vertical-security", "127.0.0.1:12345"}, {"DataCenter", "0.0.0.0:17123"}};
  EXPECT_FALSE(agent::ResolveEndpoint(config, "vertical-security", running));
  EXPECT_EQ(agent::AvailableServices(config, {}), (std::vector<std::string>{"ModuleManager"}));
  config.sshEnabled = true;
  const auto target = agent::ResolveEndpoint(config, "vertical-security", running);
  ASSERT_TRUE(target);
  EXPECT_EQ(target->host, "11.22.33.44");
  EXPECT_EQ(target->port, 8888);
  // 旧端口解析接口只表示本机服务，不能把纵密目标意外当作环回服务。
  EXPECT_FALSE(agent::ResolveTarget(config, "vertical-security", running));
  const auto module = agent::ResolveEndpoint(config, "DataCenter", running);
  ASSERT_TRUE(module);
  EXPECT_EQ(module->host, "127.0.0.1");
  EXPECT_EQ(module->port, 17123);
}

// 验证普通模块仍不能指向现场地址，服务名称也不能携带用户指定的主机或端口。
TEST(PlatformAgentTargets, ExternalEndpointsCannotBeRequestedOrSpoofed) {
  agent::Config config;
  config.sshEnabled = true;
  EXPECT_FALSE(agent::ResolveEndpoint(config, "DataCenter", {{"DataCenter", "11.22.33.44:8888"}}));
  EXPECT_FALSE(agent::ResolveEndpoint(config, "11.22.33.44:8888", {}));
  EXPECT_FALSE(agent::ResolveEndpoint(config, "vertical-security:8889", {}));
  EXPECT_FALSE(agent::ResolveEndpoint(config, "VERTICAL-SECURITY", {}));
}

namespace {
class EmptyModuleManager final : public ModuleManagerProto::ModuleManage::Service {
public:
  std::atomic<bool> *helloReceived{nullptr};
  std::atomic<bool> queriedBeforeHello{false};
  grpc::Status GetRunningModuleInfo(grpc::ServerContext *, const ModuleManagerProto::Empty *, ModuleManagerProto::ModuleRunningInfos *) override {
    if (helloReceived != nullptr && !helloReceived->load())
      queriedBeforeHello = true;
    return grpc::Status::OK;
  }
};

class HalfCloseBackend {
public:
  HalfCloseBackend() {
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener_ < 0 || bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener_, 1) != 0)
      throw std::runtime_error("创建测试环回服务失败");
    socklen_t size = sizeof(address);
    getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size);
    port = ntohs(address.sin_port);
    worker_ = std::jthread([this] {
      const int client = accept(listener_, nullptr, nullptr);
      if (client < 0)
        return;
      // 即使被测半关闭逻辑故障，测试后端也必须有限时间退出。
      timeval timeout{5, 0};
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      std::array<char, 4096> buffer{};
      std::string request;
      ssize_t count;
      while ((count = recv(client, buffer.data(), buffer.size(), 0)) > 0)
        request.append(buffer.data(), count);
      const std::string response = request + "尾部";
      size_t offset = 0;
      while (offset < response.size()) {
        const auto written = send(client, response.data() + offset, response.size() - offset, MSG_NOSIGNAL);
        if (written <= 0)
          break;
        offset += written;
      }
      shutdown(client, SHUT_WR);
      close(client);
    });
  }
  ~HalfCloseBackend() {
    shutdown(listener_, SHUT_RDWR);
    close(listener_);
  }
  uint16_t port;

private:
  int listener_;
  std::jthread worker_;
};

class FakePlatform final : public remote::DeviceAccessService::Service {
public:
  std::promise<std::string> result;
  std::atomic<bool> completed{false};
  std::atomic<bool> helloReceived{false};
  std::atomic<unsigned int> heartbeats{0};
  std::atomic<bool> heartbeatDuringTransfer{false};
  std::string payload{std::string("\0\1\2", 3)};
  bool waitForTransferHeartbeat{false};
  grpc::Status ConnectDevice(grpc::ServerContext *context, grpc::ServerReaderWriter<remote::PlatformMessage, remote::DeviceMessage> *stream) override {
    remote::DeviceMessage input;
    if (!stream->Read(&input) || !input.has_hello())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "缺少设备握手");
    helloReceived = true;
    remote::PlatformMessage welcome;
    welcome.mutable_welcome()->set_connection_id("test-connection");
    welcome.mutable_welcome()->set_heartbeat_interval_seconds(1);
    stream->Write(welcome);
    remote::PlatformMessage request;
    request.mutable_open_tunnel()->set_tunnel_id("test-tunnel");
    request.mutable_open_tunnel()->set_service("ssh");
    request.mutable_open_tunnel()->set_attach_token("test-ticket");
    stream->Write(request);
    while (stream->Read(&input)) {
      if (input.has_tunnel_failure() && !completed.exchange(true))
        result.set_value("连接失败");
      if (input.has_heartbeat()) {
        ++heartbeats;
        remote::PlatformMessage ack;
        ack.mutable_heartbeat_ack();
        stream->Write(ack);
      }
    }
    return grpc::Status::OK;
  }
  grpc::Status AttachTunnel(grpc::ServerContext *, grpc::ServerReaderWriter<remote::TunnelFrame, remote::TunnelFrame> *stream) override {
    remote::TunnelFrame frame;
    if (!stream->Read(&frame) || !frame.has_attach())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "缺少附着握手");
    frame.Clear();
    frame.mutable_ready();
    stream->Write(frame);
    for (size_t offset = 0; offset < payload.size(); offset += 65536) {
      frame.Clear();
      frame.set_data(payload.substr(offset, 65536));
      if (!stream->Write(frame))
        return grpc::Status(grpc::StatusCode::CANCELLED, "测试传输已取消");
      if (waitForTransferHeartbeat && offset == payload.size() / 2) {
        const auto before = heartbeats.load();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (heartbeats.load() == before && std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(10ms);
        heartbeatDuringTransfer = heartbeats.load() > before;
        if (!heartbeatDuringTransfer)
          return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "传输期间未收到控制心跳");
      }
    }
    frame.Clear();
    frame.mutable_half_close();
    stream->Write(frame);
    std::string response;
    while (stream->Read(&frame)) {
      if (frame.has_data())
        response += frame.data();
      if (frame.has_half_close()) {
        if (!completed.exchange(true))
          result.set_value(response);
        frame.Clear();
        frame.mutable_close();
        stream->Write(frame);
        break;
      }
    }
    return grpc::Status::OK;
  }
};

class ReconnectingPlatform final : public remote::DeviceAccessService::Service {
public:
  std::promise<bool> freshTransport;
  std::atomic<unsigned int> attempts{0};
  std::mutex peerMutex;
  std::string firstPeer;

  grpc::Status ConnectDevice(grpc::ServerContext *context, grpc::ServerReaderWriter<remote::PlatformMessage, remote::DeviceMessage> *stream) override {
    remote::DeviceMessage input;
    if (!stream->Read(&input) || !input.has_hello())
      return {grpc::StatusCode::INVALID_ARGUMENT, "缺少设备握手"};
    const auto attempt = ++attempts;
    remote::PlatformMessage welcome;
    welcome.mutable_welcome()->set_connection_id("reconnect-" + std::to_string(attempt));
    welcome.mutable_welcome()->set_heartbeat_interval_seconds(1);
    stream->Write(welcome);
    if (attempt == 1) {
      std::lock_guard lock(peerMutex);
      firstPeer = context->peer();
      return {grpc::StatusCode::UNAVAILABLE, "模拟控制连接失败"};
    }
    if (attempt == 2) {
      std::lock_guard lock(peerMutex);
      freshTransport.set_value(context->peer() != firstPeer);
    }
    while (stream->Read(&input)) {
      if (input.has_heartbeat()) {
        remote::PlatformMessage ack;
        ack.mutable_heartbeat_ack();
        stream->Write(ack);
      }
    }
    return grpc::Status::OK;
  }
};
}  // namespace

class PlatformAgentIntegration : public ::testing::Test {
public:
  static void SetUpTestSuite() {
    // 日志的文件扫描和首次动态库初始化不属于远程操作的等待期限。
    ModuleManager::Logger::ensureModuleSink("PlatformAgent");
  }
};

// 验证真实 gRPC 双向流承载二进制数据，发送方向半关闭后仍完整保留本机响应尾部。
TEST_F(PlatformAgentIntegration, PreservesHalfCloseTailAndStops) {
  HalfCloseBackend backend;
  FakePlatform service;
  EmptyModuleManager manager;
  manager.helloReceived = &service.helloReceived;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  builder.RegisterService(&manager);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  auto ready = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
  ASSERT_TRUE(ready->WaitForConnected(std::chrono::system_clock::now() + 10s));
  agent::Config config;
  config.enabled = true;
  config.target = "127.0.0.1:" + std::to_string(port);
  config.deviceId = "device-a";
  config.token = "test-token";
  config.sshEnabled = true;
  config.sshPort = backend.port;
  config.managerPort = port;
  agent::PlatformAgent client(config);
  auto result = service.result.get_future();
  std::jthread worker([&](std::stop_token stop) { client.Run(stop); });
  EXPECT_EQ(result.wait_for(10s), std::future_status::ready);
  if (result.wait_for(0s) == std::future_status::ready)
    EXPECT_EQ(result.get(), std::string("\0\1\2", 3) + "尾部");
  EXPECT_FALSE(manager.queriedBeforeHello.load()) << "设备首帧不得依赖本机模块查询";
  worker.request_stop();
  worker.join();
  server->Shutdown(std::chrono::system_clock::now() + 2s);
}

// 验证本机目标连接失败经控制流明确通知，停止连接功能不会阻塞控制流读取。
TEST_F(PlatformAgentIntegration, ReportsBackendFailureAndCancelsControlRead) {
  FakePlatform service;
  EmptyModuleManager manager;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  builder.RegisterService(&manager);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  auto ready = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
  ASSERT_TRUE(ready->WaitForConnected(std::chrono::system_clock::now() + 10s));
  agent::Config config;
  config.enabled = true;
  config.target = "127.0.0.1:" + std::to_string(port);
  config.deviceId = "device-a";
  config.token = "test-token";
  config.managerPort = port;
  // SSH 未开放，因此请求在目标解析阶段失败，不能偷偷访问本机其他服务。
  config.sshEnabled = false;
  agent::PlatformAgent client(config);
  auto result = service.result.get_future();
  std::jthread worker([&](std::stop_token stop) { client.Run(stop); });
  EXPECT_EQ(result.wait_for(10s), std::future_status::ready);
  if (result.wait_for(0s) == std::future_status::ready)
    EXPECT_EQ(result.get(), "连接失败");
  const auto begin = std::chrono::steady_clock::now();
  worker.request_stop();
  worker.join();
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 3s);
  server->Shutdown(std::chrono::system_clock::now() + 2s);
}

// 验证超过 SSH 初始窗口的 50 MiB 二进制传输完整保留，且中途控制心跳仍有进展。
TEST_F(PlatformAgentIntegration, LargeTransferKeepsControlHeartbeatAlive) {
  HalfCloseBackend backend;
  FakePlatform service;
  service.payload.resize(50 * 1024 * 1024);
  for (size_t index = 0; index < service.payload.size(); ++index)
    service.payload[index] = static_cast<char>(index % 251);
  service.waitForTransferHeartbeat = true;
  EmptyModuleManager manager;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  builder.RegisterService(&manager);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  agent::Config config;
  config.enabled = true;
  config.target = "127.0.0.1:" + std::to_string(port);
  config.deviceId = "device-large";
  config.token = "test-token";
  config.sshEnabled = true;
  config.sshPort = backend.port;
  config.managerPort = port;
  agent::PlatformAgent client(config);
  auto result = service.result.get_future();
  std::jthread worker([&](std::stop_token stop) { client.Run(stop); });
  const auto status = result.wait_for(20s);
  EXPECT_EQ(status, std::future_status::ready);
  if (status == std::future_status::ready) {
    const auto received = result.get();
    EXPECT_EQ(received.size(), service.payload.size() + std::string("尾部").size());
    EXPECT_TRUE(received == service.payload + "尾部") << "大文件内容或半关闭尾部不一致";
  }
  EXPECT_TRUE(service.heartbeatDuringTransfer.load());
  worker.request_stop();
  worker.join();
  server->Shutdown(std::chrono::system_clock::now() + 2s);
}

// 验证控制 RPC 失败后重连使用新 TCP 连接，避免在旧传输连接上反复重新握手。
TEST_F(PlatformAgentIntegration, RecreatesTransportAfterControlFailure) {
  ReconnectingPlatform service;
  EmptyModuleManager manager;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  builder.RegisterService(&manager);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  agent::Config config;
  config.enabled = true;
  config.target = "127.0.0.1:" + std::to_string(port);
  config.deviceId = "device-reconnect";
  config.token = "test-token";
  config.managerPort = port;
  agent::PlatformAgent client(config);
  auto result = service.freshTransport.get_future();
  std::jthread worker([&](std::stop_token stop) { client.Run(stop); });
  const auto status = result.wait_for(10s);
  EXPECT_EQ(status, std::future_status::ready);
  if (status == std::future_status::ready)
    EXPECT_TRUE(result.get());
  worker.request_stop();
  worker.join();
  server->Shutdown(std::chrono::system_clock::now() + 2s);
}
