#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <stdexcept>

#include "PlatformAgent.h"

namespace mskdsp::platformagent {
namespace {
bool NameValid(std::string_view name) {
  return !name.empty() && name.size() <= 64 &&
      std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
      });
}

uint16_t ParsePort(std::string_view value) {
  unsigned int port = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
  if (error != std::errc() || end != value.data() + value.size() || port == 0 || port > 65535)
    throw std::invalid_argument("平台连接功能的本机端口不合法");
  return static_cast<uint16_t>(port);
}

bool Flag(const std::string &value) {
  if (value.empty() || value == "0")
    return false;
  if (value == "1")
    return true;
  throw std::invalid_argument("平台连接功能开关必须是 0 或 1");
}

}  // namespace

Config LoadConfig(const std::function<std::string(std::string_view)> &env) {
  Config config;
  config.enabled = Flag(env("MSKDSP_PLATFORM_ENABLED"));
  if (!config.enabled)
    return config;
  config.target = env("MSKDSP_PLATFORM_TARGET");
  config.deviceId = env("MSKDSP_PLATFORM_DEVICE_ID");
  config.token = env("MSKDSP_PLATFORM_DEVICE_TOKEN");
  if (config.target.empty() || config.deviceId.empty() || config.token.empty())
    throw std::invalid_argument("启用平台连接功能必须提供平台目标、设备 ID 和设备令牌");
  if (config.deviceId.size() > 128 || config.token.size() > 4096 ||
      std::any_of(config.token.begin(), config.token.end(), [](unsigned char c) { return c < 33 || c > 126; }))
    throw std::invalid_argument("平台连接功能的设备身份格式不合法");
  const auto colon = config.target.rfind(':');
  if (colon == std::string::npos || colon == 0 || config.target.find_first_of("/\\\r\n\t ") != std::string::npos)
    throw std::invalid_argument("平台目标必须为主机名和端口");
  ParsePort(std::string_view(config.target).substr(colon + 1));
  config.sshEnabled = Flag(env("MSKDSP_PLATFORM_SSH_ENABLED"));
  if (const auto value = env("MSKDSP_PLATFORM_MANAGER_PORT"); !value.empty())
    config.managerPort = ParsePort(value);
  if (const auto value = env("MSKDSP_PLATFORM_SSH_PORT"); !value.empty())
    config.sshPort = ParsePort(value);
  if (const auto value = env("MSKDSP_PLATFORM_MODULES"); !value.empty()) {
    config.allowedModules.clear();
    size_t begin = 0;
    while (begin < value.size()) {
      const auto end = value.find(',', begin);
      const auto name = value.substr(begin, end == std::string::npos ? end : end - begin);
      if (!NameValid(name) || name == "ssh" || name == "ModuleManager")
        throw std::invalid_argument("平台模块白名单仅允许普通模块名称");
      config.allowedModules.insert(name);
      if (end == std::string::npos)
        break;
      begin = end + 1;
      if (begin == value.size())
        throw std::invalid_argument("平台模块白名单包含空名称");
    }
  }
  return config;
}

Config LoadEnvironmentConfig() {
  return LoadConfig([](std::string_view name) {
    const auto value = std::getenv(std::string(name).c_str());
    return value == nullptr ? std::string() : std::string(value);
  });
}

std::optional<uint16_t> ResolveTarget(const Config &config, std::string_view service, const std::vector<RunningModule> &running) {
  if (service == "ModuleManager")
    return config.managerPort;
  if (service == "ssh")
    return config.sshEnabled ? std::optional<uint16_t>(config.sshPort) : std::nullopt;
  if (!NameValid(service) || !config.allowedModules.contains(std::string(service)))
    return std::nullopt;
  for (const auto &module : running) {
    if (module.name != service)
      continue;
    const auto colon = module.address.rfind(':');
    if (colon == std::string::npos)
      return std::nullopt;
    const auto host = module.address.substr(0, colon);
    if (host != "0.0.0.0" && host != "127.0.0.1" && host != "localhost")
      return std::nullopt;
    try {
      return ParsePort(std::string_view(module.address).substr(colon + 1));
    } catch (const std::invalid_argument &) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::vector<std::string> AvailableServices(const Config &config, const std::vector<RunningModule> &running) {
  std::vector<std::string> result{"ModuleManager"};
  for (const auto &name : config.allowedModules) {
    if (ResolveTarget(config, name, running))
      result.push_back(name);
  }
  if (config.sshEnabled) {
    result.emplace_back("ssh");
    result.emplace_back(kVerticalSecurityService);
  }
  return result;
}

std::optional<ServiceEndpoint> ResolveEndpoint(const Config &config, std::string_view service, const std::vector<RunningModule> &running) {
  if (service == kVerticalSecurityService) {
    if (!config.sshEnabled)
      return std::nullopt;
    return ServiceEndpoint{"11.22.33.44", 8888};
  }
  if (const auto port = ResolveTarget(config, service, running))
    return ServiceEndpoint{"127.0.0.1", *port};
  return std::nullopt;
}
}  // namespace mskdsp::platformagent
