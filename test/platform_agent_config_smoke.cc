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
    require(!config.enabled && !config.sshEnabled);
    values = {{"MSKDSP_PLATFORM_ENABLED", "1"}, {"MSKDSP_PLATFORM_TARGET", "platform.example.org:50051"}, {"MSKDSP_PLATFORM_DEVICE_ID", "device-a"}, {"MSKDSP_PLATFORM_DEVICE_TOKEN", "device-token"}};
    config = agent::LoadConfig(environment);
    require(config.enabled && config.target == "platform.example.org:50051");
    values["MSKDSP_PLATFORM_TARGET"] = "platform.example.org:0";
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
    require(config.allowedModules.size() == 2);
    require(agent::ResolveTarget(config, "ModuleManager", {}) == 17000);
    require(agent::ResolveTarget(config, "DataCenter", {{"DataCenter", "0.0.0.0:17123"}}) == 17123);
    require(!agent::ResolveTarget(config, "DataCenter", {{"DataCenter", "192.168.1.2:17123"}}));
    require(!agent::ResolveTarget(config, "DataCenter", {}));
    require(!agent::ResolveTarget(config, "ssh", {}));
    // 未启用平台维护能力时，纵密固定服务不可解析，也不能作为普通模块伪造。
    require(!agent::ResolveEndpoint(config, "vertical-security", {}));
    config.allowedModules.insert("vertical-security");
    require(!agent::ResolveEndpoint(config, "vertical-security", {{"vertical-security", "127.0.0.1:9999"}}));
    config.allowedModules.erase("vertical-security");
    require(!agent::ResolveTarget(config, "127.0.0.1:22", {}));
    require(!agent::ResolveTarget(config, "../ssh", {}));
    require(!agent::ResolveTarget(config, "DataCenter", {{"DataCenter", "0.0.0.0:99999"}}));
    require(agent::AvailableServices(config, {}) == std::vector<std::string>{"ModuleManager"});
    values["MSKDSP_PLATFORM_SSH_ENABLED"] = "1";
    values["MSKDSP_PLATFORM_SSH_PORT"] = "10022";
    config = agent::LoadConfig(environment);
    require(agent::ResolveTarget(config, "ssh", {}) == 10022);
    require(agent::AvailableServices(config, {}) == (std::vector<std::string>{"ModuleManager", "ssh", "vertical-security"}));
    // 维护能力只开放固定的纵密管理服务，不允许请求自选目标或端口。
    const auto vertical = agent::ResolveEndpoint(config, "vertical-security", {{"vertical-security", "127.0.0.1:9999"}});
    require(vertical && vertical->host == "11.22.33.44" && vertical->port == 8888);
    const auto ssh = agent::ResolveEndpoint(config, "ssh", {});
    require(ssh && ssh->host == "127.0.0.1" && ssh->port == 10022);
    const auto module = agent::ResolveEndpoint(config, "DataCenter", {{"DataCenter", "0.0.0.0:17123"}});
    require(module && module->host == "127.0.0.1" && module->port == 17123);
    require(!agent::ResolveEndpoint(config, "DataCenter", {{"DataCenter", "11.22.33.44:8888"}}));
    require(!agent::ResolveEndpoint(config, "11.22.33.44:8888", {}));
    require(!agent::ResolveEndpoint(config, "vertical-security:8889", {}));
    require(!agent::ResolveEndpoint(config, "VERTICAL-SECURITY", {}));
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
