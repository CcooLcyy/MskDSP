#pragma once

#include <functional>
#include <string>
#include <vector>

namespace mskdsp::platformagent {
// 网卡为空时保留系统现有网络；平台接入启停不控制普通外网出口。
struct OrdinaryNetworkConfig {
  std::string interfaceName;
  std::string gateway;
  std::vector<std::string> dnsServers;
  bool operator==(const OrdinaryNetworkConfig &) const = default;
};
struct OrdinaryNetworkResult {
  bool applied{false};
  std::string error;
  std::string sourceAddress;
};
using OrdinaryNetworkApply = std::function<OrdinaryNetworkResult(const OrdinaryNetworkConfig &)>;
std::string ValidateOrdinaryNetwork(const OrdinaryNetworkConfig &config);
}  // namespace mskdsp::platformagent
