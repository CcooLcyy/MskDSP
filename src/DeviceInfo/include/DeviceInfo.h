#pragma once

#include <grpcpp/channel.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>

#include "DataCenter.grpc.pb.h"
#include "DeviceMetrics.hpp"
#include "ModuleInterface.h"

namespace DeviceInfo {

class DeviceInfo : public ModuleInterface::ModuleInterface {
public:
  DeviceInfo();
  ~DeviceInfo() override;

  void start(std::stop_token stopToken) override;

  // 测试或嵌入场景可注入 DataCenter Stub，生产环境默认连接 Unix socket。
  void SetDataCenterStub(
      std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);
  void SetDataCenterServerAddress(std::string address);

private:
  std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> GetStub();
  void EnsureStubLocked();
  bool RegisterDataCenter();
  void PublishMetrics();
  static std::string DefaultServerAddress();

  mutable std::mutex mutex_;
  std::shared_ptr<grpc::Channel> channel_;
  std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub_;
  std::string serverAddress_;
  std::atomic<uint32_t> connectionId_{0};
  CpuUsageTracker cpuTracker_;
};

}  // namespace DeviceInfo
