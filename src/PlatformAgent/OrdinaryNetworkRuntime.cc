#include "OrdinaryNetworkRuntime.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <boost/json.hpp>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <set>
#include <utility>

#include "Logger.h"

namespace mskdsp::platformagent {
namespace {
bool AddressValid(const std::string &value) {
  in_addr address{};
  if (::inet_pton(AF_INET, value.c_str(), &address) != 1)
    return false;
  const auto host = ntohl(address.s_addr);
  return host != 0 && host != 0xffffffffU && (host >> 24) != 127 && (host >> 28) < 14;
}

OrdinaryNetworkResult Failure(std::string message) {
  return {false, std::move(message), {}};
}

std::string Execute(const std::string &script, const std::string &payload, bool checkOnly) {
  int descriptors[2];
  if (::pipe2(descriptors, O_CLOEXEC) != 0)
    return {};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addclose(&actions, descriptors[0]);
  posix_spawn_file_actions_addclose(&actions, descriptors[1]);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes, 0);
  std::array<char *, 6> arguments{const_cast<char *>("/usr/bin/python3"), const_cast<char *>(script.c_str()), const_cast<char *>("--config"), const_cast<char *>(payload.c_str()), checkOnly ? const_cast<char *>("--check") : nullptr, nullptr};
  std::array<char *, 3> environment{const_cast<char *>("PATH=/usr/sbin:/usr/bin:/sbin:/bin"), const_cast<char *>("LC_ALL=C.UTF-8"), nullptr};
  pid_t pid{};
  const int spawned = ::posix_spawn(&pid, arguments[0], &actions, &attributes, arguments.data(), environment.data());
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  ::close(descriptors[1]);
  if (spawned != 0) {
    ::close(descriptors[0]);
    return {};
  }
  ::fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(checkOnly ? 6 : 18);
  bool exited = false;
  bool overflow = false;
  int status{};
  while (!exited && std::chrono::steady_clock::now() < deadline) {
    pollfd ready{descriptors[0], POLLIN, 0};
    ::poll(&ready, 1, 100);
    std::array<char, 2048> buffer;
    for (;;) {
      const auto count = ::read(descriptors[0], buffer.data(), buffer.size());
      if (count <= 0)
        break;
      if (output.size() + static_cast<size_t>(count) > 16384) {
        overflow = true;
        break;
      }
      output.append(buffer.data(), static_cast<size_t>(count));
    }
    if (overflow)
      break;
    const auto waited = ::waitpid(pid, &status, WNOHANG);
    exited = waited == pid || (waited < 0 && errno == ECHILD);
  }
  if (!exited) {
    // 超时同时回收 helper 及其命令子进程，下一次应用将读取持久恢复记录。
    ::kill(-pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  } else {
    // 进程可能在最后一次非阻塞读取后才写入结果，退出后继续排空管道。
    std::array<char, 2048> buffer;
    for (;;) {
      const auto count = ::read(descriptors[0], buffer.data(), buffer.size());
      if (count <= 0)
        break;
      if (output.size() + static_cast<size_t>(count) > 16384) {
        overflow = true;
        break;
      }
      output.append(buffer.data(), static_cast<size_t>(count));
    }
  }
  ::close(descriptors[0]);
  return exited && !overflow ? output : std::string{};
}
}  // namespace

std::string ValidateOrdinaryNetwork(const OrdinaryNetworkConfig &config) {
  if (config.interfaceName.empty()) {
    return config.gateway.empty() && config.dnsServers.empty() ? "" : "使用系统网络时不能指定网关或 DNS";
  }
  if (config.interfaceName.size() != 8 || config.interfaceName.substr(0, 7) != "eth0.10" ||
      config.interfaceName.back() < '1' || config.interfaceName.back() > '8')
    return "普通外网网卡必须是 eth0.101 至 eth0.108";
  if (!AddressValid(config.gateway))
    return "普通外网网关必须是有效 IPv4 单播地址";
  if (config.dnsServers.size() > 8)
    return "DNS 最多配置八个";
  std::set<std::string> unique;
  for (const auto &server : config.dnsServers) {
    if (!AddressValid(server) || !unique.insert(server).second)
      return "DNS 必须是有效且不重复的 IPv4 单播地址";
  }
  return {};
}

namespace {
OrdinaryNetworkResult RunOrdinaryNetwork(const OrdinaryNetworkConfig &config, bool checkOnly) {
  if (auto error = ValidateOrdinaryNetwork(config); !error.empty())
    return Failure(error);
  static std::mutex executionMutex;
  std::unique_lock lock(executionMutex, std::defer_lock);
  if (checkOnly) {
    if (!lock.try_lock())
      return Failure("普通外网配置正在应用，暂不能检查实际状态");
  } else {
    lock.lock();
  }
  try {
    // 未启用且从未管理过网络时，不依赖 Python、系统总线或网络权限。
    if (config.interfaceName.empty() && !std::filesystem::exists("./conf/ordinary_network_state.json"))
      return {true, {}, {}};
    std::filesystem::path script = "/opt/mskdsp/script/network/ordinary_network.py";
    if (!std::filesystem::is_regular_file(script))
      script = "./script/network/ordinary_network.py";
    if (!std::filesystem::is_regular_file(script))
      return Failure("缺少普通外网运行适配器，请升级完整下位机镜像");
    if (!checkOnly)
      LOG_INFO("开始应用普通外网出口，网卡={}，DNS数量={}", config.interfaceName.empty() ? "系统现有网络" : config.interfaceName, config.dnsServers.size());
    boost::json::array dns;
    for (const auto &address : config.dnsServers)
      dns.emplace_back(address);
    const auto payload = boost::json::serialize(boost::json::object{{"interface_name", config.interfaceName}, {"gateway", config.gateway}, {"dns_servers", std::move(dns)}});
    const auto output = Execute(script.string(), payload, checkOnly);
    if (output.empty()) {
      if (checkOnly)
        return Failure("普通外网实际状态检查失败或超时，请检查运行环境");
      LOG_ERROR("普通外网执行失败或超时，需检查持久恢复记录");
      return Failure("网络执行失败或超时，可能保留待恢复记录，请重新读取并重试");
    }
    const auto object = boost::json::parse(output).as_object();
    OrdinaryNetworkResult result{object.at("applied").as_bool(), std::string(object.at("error").as_string()), std::string(object.at("source_address").as_string())};
    if (!checkOnly) {
      if (result.applied)
        LOG_INFO("普通外网出口应用完成，源地址={}", result.sourceAddress);
      else
        LOG_ERROR("普通外网出口应用失败，原因={}", result.error);
    }
    return result;
  } catch (...) {
    return Failure("无法执行或解析普通外网应用结果，请检查下位机运行环境");
  }
}
}  // namespace

OrdinaryNetworkResult ApplyOrdinaryNetwork(const OrdinaryNetworkConfig &config) {
  return RunOrdinaryNetwork(config, false);
}

OrdinaryNetworkResult CheckOrdinaryNetwork(const OrdinaryNetworkConfig &config) {
  return RunOrdinaryNetwork(config, true);
}
}  // namespace mskdsp::platformagent
