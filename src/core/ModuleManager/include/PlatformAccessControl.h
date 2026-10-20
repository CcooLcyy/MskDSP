#pragma once

#include <grpcpp/support/status.h>

#include "ModuleManager.pb.h"

namespace ModuleManager {
// 管理服务仅依赖控制接口，平台连接与持久化由主进程注入。
class PlatformAccessControl {
public:
  virtual ~PlatformAccessControl() = default;
  virtual grpc::Status Get(ModuleManagerProto::PlatformAccessStatus *status) const = 0;
  virtual grpc::Status Set(const ModuleManagerProto::SetPlatformAccessConfigRequest &request, ModuleManagerProto::PlatformAccessStatus *status) = 0;
};
}  // namespace ModuleManager
