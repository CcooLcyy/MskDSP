#include "PlatformAccessSupervisor.h"

#include <sys/utsname.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <thread>

#include "Logger.h"

namespace mskdsp::platformagent {
std::string NormalizePlatformDeviceArchitecture(std::string_view machine) {
  std::string normalized(machine);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  if (normalized == "aarch64" || normalized == "arm64")
    return "arm64";
  if (normalized == "x86_64" || normalized == "amd64" || normalized == "x64")
    return "x64";
  return machine.empty() ? "unknown" : std::string(machine);
}

namespace {
std::string RuntimeDeviceArchitecture() {
  struct utsname runtime{};
  if (::uname(&runtime) != 0) {
    LOG_WARNING("无法读取下位机运行架构，平台接入状态将报告 unknown");
    return "unknown";
  }
  return NormalizePlatformDeviceArchitecture(runtime.machine);
}

bool SameConfiguration(const Config &a, const Config &b) {
  return a.enabled == b.enabled && a.deviceId == b.deviceId && a.token == b.token &&
      a.sshEnabled == b.sshEnabled && a.sshPort == b.sshPort && a.networkConfig == b.networkConfig;
}
}  // namespace

PlatformAccessSupervisor::PlatformAccessSupervisor(std::filesystem::path path, PlatformAccessRunner runner, OrdinaryNetworkApply networkApply, OrdinaryNetworkApply networkCheck) : store_(std::move(path)), runner_(std::move(runner)), networkApply_(std::move(networkApply)), networkCheck_(std::move(networkCheck)), deviceArchitecture_(RuntimeDeviceArchitecture()) {
  LOG_INFO("平台接入状态使用下位机运行架构={}", deviceArchitecture_);
  auto loaded = store_.Load();
  saved_ = std::move(loaded.saved);
  lastError_ = std::move(loaded.error);
  persisted_ = loaded.exists && lastError_.empty();
  // 崩溃可能发生在网络已应用但主配置尚未保存之间；存在执行日志时需恢复或清理。
  std::error_code stateError;
  const bool networkStateExists = std::filesystem::exists("./conf/ordinary_network_state.json", stateError);
  networkApplied_ = saved_.config.networkConfig.interfaceName.empty() && !networkStateExists && !stateError;
  if (!networkApply_)
    networkApply_ = ApplyOrdinaryNetwork;
  if (!networkCheck_)
    networkCheck_ = CheckOrdinaryNetwork;
  if (!lastError_.empty())
    LOG_ERROR("{}", lastError_);
  if (!runner_)
    runner_ = [](const Config &config, std::stop_token stop, PlatformStatusObserver observer) {
      PlatformAgent(config, std::move(observer)).Run(stop);
    };
}

OrdinaryNetworkResult PlatformAccessSupervisor::CheckNetwork(const OrdinaryNetworkConfig &config) const {
  try {
    return networkCheck_(config);
  } catch (...) {
    return {false, "普通外网状态检查异常，请检查网络运行环境", ""};
  }
}

OrdinaryNetworkResult PlatformAccessSupervisor::ApplyNetwork(const OrdinaryNetworkConfig &config) const {
  try {
    return networkApply_(config);
  } catch (...) {
    return {false, "普通外网配置执行异常，请检查网络运行环境", ""};
  }
}

void PlatformAccessSupervisor::RecordNetworkResult(const OrdinaryNetworkResult &result) {
  networkApplied_ = result.applied;
  networkError_ = result.error;
  networkSourceAddress_ = result.sourceAddress;
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
  auto *network = status->mutable_network_config();
  network->set_interface_name(saved_.config.networkConfig.interfaceName);
  network->set_gateway(saved_.config.networkConfig.gateway);
  for (const auto &dns : saved_.config.networkConfig.dnsServers)
    network->add_dns_servers(dns);
  status->set_network_applied(networkApplied_);
  status->set_network_error(networkError_);
  status->set_network_source_address(networkSourceAddress_);
  status->set_device_architecture(deviceArchitecture_);
}

grpc::Status PlatformAccessSupervisor::Get(ModuleManagerProto::PlatformAccessStatus *status) const {
  // 慢保存或后台恢复不能阻塞读取；忙碌时明确表示本次未完成核验。
  std::unique_lock saveLock(saveMutex_, std::try_to_lock);
  OrdinaryNetworkConfig config;
  {
    std::lock_guard lock(mutex_);
    FillStatus(status);
    config = saved_.config.networkConfig;
  }
  if (!saveLock.owns_lock()) {
    status->set_network_applied(false);
    status->set_network_error("网络配置正在应用或核验，请稍后重新读取");
    status->clear_network_source_address();
    return grpc::Status::OK;
  }
  const auto checked = CheckNetwork(config);
  status->set_network_applied(checked.applied);
  status->set_network_error(checked.error);
  status->set_network_source_address(checked.sourceAddress);
  return grpc::Status::OK;
}

grpc::Status PlatformAccessSupervisor::Set(const ModuleManagerProto::SetPlatformAccessConfigRequest &request, ModuleManagerProto::PlatformAccessStatus *status) {
  // 不排队等待慢后台恢复，避免累计执行时间超过上位机保存请求期限。
  std::unique_lock saveLock(saveMutex_, std::defer_lock);
  if (!saveLock.try_lock_for(std::chrono::seconds(1))) {
    LOG_INFO("普通外网执行器繁忙，平台配置本次未保存，请恢复后重试");
    return {grpc::StatusCode::UNAVAILABLE, "普通外网配置正在应用或恢复，本次未保存，请稍后重新读取并重试"};
  }
  StoredPlatformAccess previous;
  bool persisted, paused, networkReady;
  {
    std::lock_guard lock(mutex_);
    previous = saved_;
    persisted = persisted_;
    paused = paused_;
    networkReady = networkApplied_ && networkError_.empty();
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
  // 旧客户端省略消息时保留网络设置；显式空消息表示恢复系统现有网络。
  if (request.has_network_config()) {
    next.config.networkConfig.interfaceName = request.network_config().interface_name();
    next.config.networkConfig.gateway = request.network_config().gateway();
    next.config.networkConfig.dnsServers.assign(request.network_config().dns_servers().begin(), request.network_config().dns_servers().end());
  }
  if (request.clear_device_token())
    next.config.token.clear();
  else if (!request.device_token().empty())
    next.config.token = request.device_token();
  if (auto error = ValidatePlatformAccess(next.config); !error.empty())
    return {grpc::StatusCode::INVALID_ARGUMENT, error};
  if (previous.config.networkConfig == next.config.networkConfig) {
    auto checked = CheckNetwork(next.config.networkConfig);
    networkReady = checked.applied && checked.error.empty();
    if (persisted && !paused && SameConfiguration(previous.config, next.config)) {
      if (!networkReady) {
        LOG_INFO("相同配置重新保存时发现普通外网失效，开始补齐自有网络资源");
        checked = ApplyNetwork(next.config.networkConfig);
        if (checked.applied && checked.error.empty())
          LOG_INFO("相同配置的普通外网资源已恢复，源地址={}", checked.sourceAddress);
        else
          LOG_ERROR("相同配置的普通外网资源恢复失败：{}", checked.error);
      }
      std::lock_guard lock(mutex_);
      RecordNetworkResult(checked);
      FillStatus(status);
      return checked.applied && checked.error.empty() ? grpc::Status::OK : grpc::Status{grpc::StatusCode::FAILED_PRECONDITION, "普通外网出口恢复失败，请读取网络状态后重试"};
    }
    std::lock_guard lock(mutex_);
    RecordNetworkResult(checked);
  }
  if (previous.revision == std::numeric_limits<uint64_t>::max())
    return {grpc::StatusCode::FAILED_PRECONDITION, "平台接入配置修订已耗尽"};
  next.revision = previous.revision + 1;
  const bool applyNetwork = !networkReady || !(previous.config.networkConfig == next.config.networkConfig);
  OrdinaryNetworkResult networkResult;
  // 运行适配器负责原子应用；失败时再次恢复旧配置，持久修订保持不变。
  auto restoreNetwork = [&](const std::string &reason) {
    auto restored = ApplyNetwork(previous.config.networkConfig);
    restored.error = reason + (restored.applied ? "，已恢复原网络配置" : "；原网络恢复失败：" + restored.error);
    {
      std::lock_guard lock(mutex_);
      RecordNetworkResult(restored);
      FillStatus(status);
    }
    LOG_ERROR("{}", restored.error);
  };
  if (applyNetwork) {
    LOG_INFO("开始应用设备普通外网出口配置");
    networkResult = ApplyNetwork(next.config.networkConfig);
    if (!networkResult.applied) {
      restoreNetwork(networkResult.error.empty() ? "普通外网出口应用失败" : networkResult.error);
      return {grpc::StatusCode::FAILED_PRECONDITION, "普通外网出口应用失败，请读取网络状态后重试"};
    }
  }
  PlatformAccessSave result;
  try {
    result = store_.Save(next);
  } catch (...) {
    if (applyNetwork)
      restoreNetwork("平台与网络配置保存失败");
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
    if (applyNetwork)
      RecordNetworkResult(networkResult);
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
      // 冷启动时接口或宿主机 DNS 服务可能尚未就绪；失败配置定时重试且可立即停止。
      event_.wait_for(lock, stop, std::chrono::seconds(5), [&] { return applied != generation_; });
      if (stop.stop_requested())
        break;
      generation = generation_;
      config = saved_.config;
      if (paused_)
        config.enabled = false;
    }
    bool networkReady = false;
    // 检查、补齐与保存串行化，防止旧配置的恢复覆盖刚保存的新配置。
    {
      std::lock_guard saveLock(saveMutex_);
      {
        std::lock_guard lock(mutex_);
        if (generation != generation_)
          continue;
      }
      auto checked = CheckNetwork(config.networkConfig);
      bool previouslyApplied;
      std::string previousError;
      {
        std::lock_guard lock(mutex_);
        previouslyApplied = networkApplied_;
        previousError = networkError_;
        RecordNetworkResult(checked);
      }
      if (!checked.applied) {
        auto restored = ApplyNetwork(config.networkConfig);
        {
          std::lock_guard lock(mutex_);
          if (restored.applied)
            LOG_INFO("普通外网自有网络资源已恢复，源地址={}", restored.sourceAddress);
          else if (previouslyApplied || restored.error != previousError)
            LOG_ERROR("普通外网资源恢复失败：{}", restored.error);
          RecordNetworkResult(restored);
        }
        checked = std::move(restored);
      }
      networkReady = checked.applied && checked.error.empty();
    }
    // 仅网络维护不能重启平台连接任务或改变修订；新配置才回收旧任务。
    if (applied == generation && worker.joinable())
      continue;
    if (applied != generation && worker.joinable()) {
      worker.request_stop();
      worker.join();
    }
    applied = generation;
    if (!config.enabled || !networkReady)
      continue;
    {
      std::lock_guard lock(mutex_);
      if (generation != generation_ || stop.stop_requested())
        continue;
      online_ = false;
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
