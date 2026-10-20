#include "PlatformAccessSupervisor.h"

#include <limits>
#include <thread>

#include "Logger.h"

namespace mskdsp::platformagent {
namespace {
bool SameConfiguration(const Config &a, const Config &b) {
  return a.enabled == b.enabled && a.deviceId == b.deviceId && a.token == b.token &&
      a.sshEnabled == b.sshEnabled && a.sshPort == b.sshPort;
}
}  // namespace

PlatformAccessSupervisor::PlatformAccessSupervisor(std::filesystem::path path, PlatformAccessRunner runner) : store_(std::move(path)), runner_(std::move(runner)) {
  auto loaded = store_.Load();
  saved_ = std::move(loaded.saved);
  lastError_ = std::move(loaded.error);
  persisted_ = loaded.exists && lastError_.empty();
  if (!lastError_.empty())
    LOG_ERROR("{}", lastError_);
  if (!runner_)
    runner_ = [](const Config &config, std::stop_token stop, PlatformStatusObserver observer) {
      PlatformAgent(config, std::move(observer)).Run(stop);
    };
}

void PlatformAccessSupervisor::FillStatus(ModuleManagerProto::PlatformAccessStatus *status) const {
  status->Clear();
  status->set_enabled(saved_.config.enabled);
  status->set_platform_target(kPlatformTarget);
  status->set_device_id(saved_.config.deviceId);
  status->set_token_configured(!saved_.config.token.empty());
  status->set_ssh_enabled(saved_.config.sshEnabled);
  status->set_ssh_port(saved_.config.sshPort);
  status->set_revision(saved_.revision);
  status->set_online(online_);
  status->set_last_error(lastError_);
}

grpc::Status PlatformAccessSupervisor::Get(ModuleManagerProto::PlatformAccessStatus *status) const {
  std::lock_guard lock(mutex_);
  FillStatus(status);
  return grpc::Status::OK;
}

grpc::Status PlatformAccessSupervisor::Set(const ModuleManagerProto::SetPlatformAccessConfigRequest &request, ModuleManagerProto::PlatformAccessStatus *status) {
  // 保存串行化与运行状态分开，文件IO不阻塞Get和连接状态回调。
  std::lock_guard saveLock(saveMutex_);
  StoredPlatformAccess previous;
  bool persisted, paused;
  {
    std::lock_guard lock(mutex_);
    previous = saved_;
    persisted = persisted_;
    paused = paused_;
  }
  if (request.expected_revision() != previous.revision)
    return {grpc::StatusCode::ABORTED, "平台接入配置已变化，请重新读取"};
  if (request.clear_device_token() && !request.device_token().empty())
    return {grpc::StatusCode::INVALID_ARGUMENT, "新令牌与清除令牌不能同时指定"};
  if (request.device_id() != previous.config.deviceId && !previous.config.token.empty() &&
      request.device_token().empty() && !request.clear_device_token())
    return {grpc::StatusCode::INVALID_ARGUMENT, "更换设备ID时必须提供新令牌或显式清除旧令牌"};
  if (request.ssh_port() > 65535)
    return {grpc::StatusCode::INVALID_ARGUMENT, "SSH端口必须为1至65535或0使用默认值"};
  auto next = previous;
  next.config.enabled = request.enabled();
  next.config.deviceId = request.device_id();
  next.config.sshEnabled = request.ssh_enabled();
  next.config.sshPort = request.ssh_port() == 0 ? 22 : static_cast<uint16_t>(request.ssh_port());
  if (request.clear_device_token())
    next.config.token.clear();
  else if (!request.device_token().empty())
    next.config.token = request.device_token();
  if (auto error = ValidatePlatformAccess(next.config); !error.empty())
    return {grpc::StatusCode::INVALID_ARGUMENT, error};
  if (persisted && !paused && SameConfiguration(previous.config, next.config))
    return Get(status);
  if (previous.revision == std::numeric_limits<uint64_t>::max())
    return {grpc::StatusCode::FAILED_PRECONDITION, "平台接入配置修订已耗尽"};
  next.revision = previous.revision + 1;
  PlatformAccessSave result;
  try {
    result = store_.Save(next);
  } catch (...) {
    LOG_ERROR("平台接入配置保存失败，当前修订={}", previous.revision);
    return {grpc::StatusCode::INTERNAL, "平台接入配置保存失败，原配置继续生效"};
  }
  {
    std::lock_guard lock(mutex_);
    saved_ = std::move(next);
    ++generation_;
    online_ = false;
    persisted_ = true;
    paused_ = result != PlatformAccessSave::Durable;
    lastError_ = paused_ ? "配置已替换但目录同步失败，耐久性不确定，接入已暂停，请重新保存" : "";
    FillStatus(status);
  }
  event_.notify_all();
  LOG_INFO("平台接入配置已保存，修订={}，接入={}，SSH={}", status->revision(), status->enabled(), status->ssh_enabled());
  if (result != PlatformAccessSave::Durable)
    return {grpc::StatusCode::INTERNAL, "配置已替换但目录同步失败，请重新读取修订并保存"};
  return grpc::Status::OK;
}

void PlatformAccessSupervisor::Run(std::stop_token stop) {
  std::jthread worker;
  uint64_t applied = std::numeric_limits<uint64_t>::max();
  while (!stop.stop_requested()) {
    Config config;
    uint64_t generation;
    {
      std::unique_lock lock(mutex_);
      event_.wait(lock, stop, [&] { return applied != generation_; });
      if (stop.stop_requested())
        break;
      generation = generation_;
      config = saved_.config;
      if (paused_)
        config.enabled = false;
      online_ = false;
    }
    // 回收时不持有状态锁，旧任务回调和RPC均可继续完成。
    if (worker.joinable()) {
      worker.request_stop();
      worker.join();
    }
    applied = generation;
    if (!config.enabled)
      continue;
    {
      std::lock_guard lock(mutex_);
      if (generation != generation_ || stop.stop_requested())
        continue;
    }
    worker = std::jthread([this, config, generation](std::stop_token token) {
      ModuleManager::LogModuleScope logScope("PlatformAgent");
      auto observer = [this, generation](bool online, std::string error) {
        std::lock_guard lock(mutex_);
        if (generation != generation_)
          return;
        online_ = online;
        lastError_ = std::move(error);
      };
      try {
        runner_(config, token, observer);
        observer(false, "");
      } catch (...) {
        LOG_ERROR("平台连接任务异常退出，修订对应代次={}", generation);
        std::lock_guard lock(mutex_);
        if (generation == generation_) {
          online_ = false;
          paused_ = true;
          lastError_ = "平台连接任务异常退出，请重新保存配置";
        }
      }
    });
  }
  if (worker.joinable()) {
    worker.request_stop();
    worker.join();
  }
  std::lock_guard lock(mutex_);
  online_ = false;
}
}  // namespace mskdsp::platformagent
