#include <iostream>
#include <map>
#include <stdexcept>

#include "PlatformAgent.h"

namespace agent = mskdsp::platformagent;

// 此纯逻辑测试只需标准库，可在本机现有编译器上验证真实配置与目标解析行为。
int main() {
  auto require = [](bool condition) {
    if (!condition)
      throw std::runtime_error("平台连接配置行为检查失败");
  };
  try {
    std::map<std::string, std::string> values;
    const auto environment = [&](std::string_view name) { return values[std::string(name)]; };
    auto config = agent::LoadConfig(environment);
    require(!config.enabled && !config.insecure && !config.sshEnabled);
    values = {{"MSKDSP_PLATFORM_ENABLED", "1"}, {"MSKDSP_PLATFORM_TARGET", "platform.example.org:50051"}, {"MSKDSP_PLATFORM_DEVICE_ID", "device-a"}, {"MSKDSP_PLATFORM_DEVICE_TOKEN", "device-token"}};
    config = agent::LoadConfig(environment);
    require(config.enabled && !config.insecure);
    values["MSKDSP_PLATFORM_INSECURE"] = "1";
    bool rejected = false;
    try {
      agent::LoadConfig(environment);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    require(rejected);
    values["MSKDSP_PLATFORM_TARGET"] = "127.0.0.1:50051";
    values["MSKDSP_PLATFORM_MODULES"] = "DataCenter,IEC104";
    config = agent::LoadConfig(environment);
    require(config.insecure && config.allowedModules.size() == 2);
    require(agent::ResolveTarget(config, "ModuleManager", {}) == 17000);
    require(agent::ResolveTarget(config, "DataCenter", {{"DataCenter", "0.0.0.0:17123"}}) == 17123);
    require(!agent::ResolveTarget(config, "DataCenter", {{"DataCenter", "192.168.1.2:17123"}}));
    require(!agent::ResolveTarget(config, "DataCenter", {}));
    require(!agent::ResolveTarget(config, "ssh", {}));
    require(!agent::ResolveTarget(config, "127.0.0.1:22", {}));
    require(!agent::ResolveTarget(config, "../ssh", {}));
    require(!agent::ResolveTarget(config, "DataCenter", {{"DataCenter", "0.0.0.0:99999"}}));
    require(agent::AvailableServices(config, {}) == std::vector<std::string>{"ModuleManager"});
    values["MSKDSP_PLATFORM_SSH_ENABLED"] = "1";
    values["MSKDSP_PLATFORM_SSH_PORT"] = "10022";
    config = agent::LoadConfig(environment);
    require(agent::ResolveTarget(config, "ssh", {}) == 10022);
    require(agent::AvailableServices(config, {}) == (std::vector<std::string>{"ModuleManager", "ssh"}));
    values["MSKDSP_PLATFORM_MODULES"] = "DataCenter,127.0.0.1:22";
    rejected = false;
    try {
      agent::LoadConfig(environment);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    require(rejected);
    std::cout << "平台连接纯配置及目标解析行为检查通过\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
