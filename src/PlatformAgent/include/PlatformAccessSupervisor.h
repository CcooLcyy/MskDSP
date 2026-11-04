#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string_view>

#include "OrdinaryNetworkRuntime.h"
#include "PlatformAccessControl.h"
#include "PlatformAccessStore.h"

namespace mskdsp::platformagent {
// 仅归一化已知架构别名；未知探测值原样保留，空值返回 unknown。
std::string NormalizePlatformDeviceArchitecture(std::string_view machine);
using PlatformAccessRunner = std::function<void(const Config &, std::stop_token, PlatformStatusObserver)>;
// 单个控制线程拥有连接任务；RPC仅在持久化成功后通知，不持锁回收线程。
class PlatformAccessSupervisor : public ModuleManager::PlatformAccessControl {
public:
  explicit PlatformAccessSupervisor(std::filesystem::path path, PlatformAccessRunner runner = {}, OrdinaryNetworkApply networkApply = {}, OrdinaryNetworkApply networkCheck = {});
  grpc::Status Get(ModuleManagerProto::PlatformAccessStatus *status) const override;
  grpc::Status Set(const ModuleManagerProto::SetPlatformAccessConfigRequest &request, ModuleManagerProto::PlatformAccessStatus *status) override;
  void Run(std::stop_token stop);

private:
  void FillStatus(ModuleManagerProto::PlatformAccessStatus *status) const;
  PlatformAccessStore store_;
  PlatformAccessRunner runner_;
  OrdinaryNetworkApply networkApply_;
  OrdinaryNetworkApply networkCheck_;
  const std::string deviceArchitecture_;
  OrdinaryNetworkResult ApplyNetwork(const OrdinaryNetworkConfig &config) const;
  OrdinaryNetworkResult CheckNetwork(const OrdinaryNetworkConfig &config) const;
  void RecordNetworkResult(const OrdinaryNetworkResult &result);
  mutable std::timed_mutex saveMutex_;
  mutable std::mutex mutex_;
  std::condition_variable_any event_;
  StoredPlatformAccess saved_;
  uint64_t generation_{0};
  bool online_{false};
  bool persisted_{false};
  bool paused_{false};
  std::string lastError_;
  bool networkApplied_{true};
  std::string networkError_;
  std::string networkSourceAddress_;
};
}  // namespace mskdsp::platformagent
