#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "OrdinaryNetworkConfig.h"

namespace mskdsp::platformagent {

struct Config {
  bool enabled{false};
  bool sshEnabled{true};
  std::string target;
  std::string deviceId;
  std::string token;
  uint16_t managerPort{17000};
  uint16_t sshPort{10022};
  OrdinaryNetworkConfig networkConfig;
  std::set<std::string> allowedModules{"DataCenter", "IEC104", "IEC61850", "ModbusRTU", "ModbusTCP", "DLT645", "AGC", "AVC", "Calc", "ControlOrchestrator", "ConfigPusher", "MQTTManager", "BoardIO", "DeviceInfo"};
};

struct RunningModule {
  std::string name;
  std::string address;
};

// 只有固定维护服务可以使用非环回地址；调用方不能传入自选主机。
struct ServiceEndpoint {
  std::string host;
  uint16_t port;
};

inline constexpr std::string_view kVerticalSecurityService = "vertical-security";

Config LoadConfig(const std::function<std::string(std::string_view)> &environment);
Config LoadEnvironmentConfig();
std::optional<uint16_t> ResolveTarget(const Config &config, std::string_view service, const std::vector<RunningModule> &running);
std::optional<ServiceEndpoint> ResolveEndpoint(const Config &config, std::string_view service, const std::vector<RunningModule> &running);
std::vector<std::string> AvailableServices(const Config &config, const std::vector<RunningModule> &running);

using PlatformStatusObserver = std::function<void(bool, std::string)>;

// 连接任务由持久配置控制器管理，不重放断线前的任何业务字节。
class PlatformAgent {
public:
  explicit PlatformAgent(Config config, PlatformStatusObserver observer = {});
  void Run(std::stop_token stop);

private:
  Config config_;
  PlatformStatusObserver observer_;
};

}  // namespace mskdsp::platformagent
