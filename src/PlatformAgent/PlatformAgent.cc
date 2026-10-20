#include <algorithm>
#include <chrono>
#include <thread>

#include "Logger.h"
#include "ModuleManager.grpc.pb.h"
#include "PlatformAgentInternal.h"
#include "ThreadUtil.hpp"

namespace mskdsp::platformagent {
using namespace std::chrono_literals;

int64_t SteadyMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::vector<RunningModule> DiscoverModules(const Config &config) {
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(config.managerPort), grpc::InsecureChannelCredentials());
  auto stub = ModuleManagerProto::ModuleManage::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 2s);
  ModuleManagerProto::Empty request;
  ModuleManagerProto::ModuleRunningInfos response;
  LOG_DEBUG("平台接入发送本机运行模块查询");
  const auto status = stub->GetRunningModuleInfo(&context, request, &response);
  std::vector<RunningModule> result;
  if (!status.ok()) {
    LOG_WARNING("平台接入查询本机运行模块失败，状态码={}", static_cast<int>(status.error_code()));
    return result;
  }
  for (const auto &item : response.module_running_info())
    result.push_back({item.module_name(), item.outer_grpc_server()});
  LOG_DEBUG("平台接入收到本机运行模块查询，模块数={}", result.size());
  return result;
}

bool ControlConnection::Send(const remote::DeviceMessage &message) {
  std::lock_guard lock(writeMutex);
  if (!active.load())
    return false;
  writeDeadline = SteadyMilliseconds() + 30000;
  const bool result = stream->Write(message);
  writeDeadline = 0;
  if (!result)
    Cancel();
  return result;
}

void ControlConnection::Cancel() {
  active = false;
  context.TryCancel();
}

PlatformAgent::PlatformAgent(Config config) : config_(std::move(config)) {}

void PlatformAgent::Run(std::stop_token stop) {
  if (!config_.enabled) {
    LOG_INFO("平台连接功能未启用");
    return;
  }
  // 冷启动日志准备必须在控制流首帧期限之外完成。
  ModuleManager::Logger::ensureModuleSink("PlatformAgent");
  grpc::ChannelArguments arguments;
  arguments.SetMaxReceiveMessageSize(128 * 1024);
  auto channel = grpc::CreateCustomChannel(config_.target, grpc::InsecureChannelCredentials(), arguments);
  auto stub = remote::DeviceAccessService::NewStub(channel);
  unsigned int failures = 0;
  while (!stop.stop_requested()) {
    auto control = std::make_shared<ControlConnection>();
    control->lastReceived = SteadyMilliseconds();
    control->context.AddMetadata("authorization", "Bearer " + config_.token);
    std::stop_callback cancelOnStop(stop, [&] { control->Cancel(); });
    auto watchdog = ModuleManager::StartModuleThread("PlatformAgent", [&](std::stop_token token) {
      while (!token.stop_requested() && control->active.load()) {
        const auto now = SteadyMilliseconds();
        const auto write = control->writeDeadline.load();
        const auto limit = control->welcomed.load() ? 90000 : 10000;
        if (stop.stop_requested() || (write != 0 && now >= write) ||
            now - control->lastReceived.load() >= limit) {
          LOG_INFO("平台控制连接停止或超时，设备={}", config_.deviceId);
          control->Cancel();
          break;
        }
        std::this_thread::sleep_for(100ms);
      }
    });
    control->stream = stub->ConnectDevice(&control->context);
    remote::DeviceMessage hello;
    hello.mutable_hello()->set_device_id(config_.deviceId);
    hello.mutable_hello()->set_agent_version("platform-agent.v1");
    // 首帧不依赖本机 RPC；运行模块由欢迎后的立即心跳完整刷新。
    for (const auto &service : AvailableServices(config_, {}))
      hello.mutable_hello()->add_services(service);
    LOG_INFO("平台接入发送设备握手，设备={}，服务数={}", config_.deviceId, hello.hello().services_size());
    control->Send(hello);
    auto heartbeat = ModuleManager::StartModuleThread("PlatformAgent", [&](std::stop_token token) {
      auto next = SteadyMilliseconds();
      while (!token.stop_requested() && control->active.load()) {
        if (control->welcomed.load() && SteadyMilliseconds() >= next) {
          remote::DeviceMessage message;
          for (const auto &service : AvailableServices(config_, DiscoverModules(config_)))
            message.mutable_heartbeat()->add_services(service);
          LOG_DEBUG("平台接入发送心跳，设备={}，服务数={}", config_.deviceId, message.heartbeat().services_size());
          if (!control->Send(message))
            break;
          next = SteadyMilliseconds() + control->heartbeatSeconds.load() * 1000;
        }
        std::this_thread::sleep_for(100ms);
      }
    });
    struct Task {
      std::shared_ptr<TunnelConnection> connection;
      std::jthread thread;
      Task(std::shared_ptr<TunnelConnection> state, std::jthread worker) : connection(std::move(state)), thread(std::move(worker)) {}
      Task(Task &&) noexcept = default;
      Task &operator=(Task &&other) noexcept {
        // 压缩已完成槽位时先取消旧连接，再回收线程，避免依赖容器搬移细节。
        if (this != &other) {
          if (connection)
            connection->Cancel();
          if (thread.joinable())
            thread.join();
          connection = std::move(other.connection);
          thread = std::move(other.thread);
        }
        return *this;
      }
      ~Task() {
        if (connection)
          connection->Cancel();
        if (thread.joinable())
          thread.join();
      }
    };
    std::vector<Task> tasks;
    tasks.reserve(16);
    remote::PlatformMessage message;
    while (control->active.load() && control->stream->Read(&message)) {
      if (!control->welcomed.load()) {
        if (!message.has_welcome() || message.welcome().connection_id().empty()) {
          LOG_WARNING("平台控制首帧不是有效欢迎消息");
          break;
        }
        control->heartbeatSeconds = std::clamp(message.welcome().heartbeat_interval_seconds(), 1u, 30u);
        control->welcomed = true;
        failures = 0;
        LOG_INFO("平台设备连接功能已上线，设备={}", config_.deviceId);
      } else if (message.has_open_tunnel()) {
        std::erase_if(tasks, [](Task &task) { return task.connection->completed.load(); });
        const auto request = message.open_tunnel();
        LOG_INFO("平台接入收到隧道通知，隧道={}，服务={}", request.tunnel_id(), request.service());
        if (request.tunnel_id().empty() || request.attach_token().empty() || tasks.size() >= 16) {
          remote::DeviceMessage failure;
          failure.mutable_tunnel_failure()->set_tunnel_id(request.tunnel_id());
          failure.mutable_tunnel_failure()->set_message("隧道参数无效或连接数量已达上限");
          control->Send(failure);
        } else {
          auto tunnel = std::make_shared<TunnelConnection>();
          auto worker = ModuleManager::StartModuleThread("PlatformAgent", [&, tunnel, request] {
            try {
              RunTunnel(config_, stub.get(), request, control, tunnel);
            } catch (...) {
              LOG_ERROR("平台隧道线程异常，隧道={}", request.tunnel_id());
              tunnel->Cancel();
              tunnel->CloseSocket();
            }
            tunnel->completed = true;
          });
          tasks.push_back({tunnel, std::move(worker)});
        }
      } else if (!message.has_heartbeat_ack()) {
        LOG_WARNING("平台控制连接收到不允许的消息类型");
        break;
      }
      control->lastReceived = SteadyMilliseconds();
    }
    control->Cancel();
    for (auto &task : tasks)
      task.connection->Cancel();
    tasks.clear();
    heartbeat.request_stop();
    heartbeat.join();
    watchdog.request_stop();
    watchdog.join();
    const auto status = control->stream->Finish();
    LOG_INFO("平台设备连接已关闭，设备={}，状态码={}", config_.deviceId, static_cast<int>(status.error_code()));
    const auto delay = std::min(30u, 1u << std::min(failures++, 5u));
    for (unsigned int count = 0; count < delay * 10 && !stop.stop_requested(); ++count)
      std::this_thread::sleep_for(100ms);
  }
}
}  // namespace mskdsp::platformagent
