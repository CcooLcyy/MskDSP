#pragma once
#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/sync_stream.h>

#include <mutex>

#include "ModuleManager.grpc.pb.h"
#include "ModuleManager.h"
#include "ModuleManager.pb.h"
#include "PlatformAccessControl.h"

namespace ModuleManager {
class ModuleManagerServiceImpl : public ModuleManagerProto::ModuleManage::Service {
public:
  void getModuleManager(ModuleManager *moduleManager);
  // 获取模块信息
  grpc::Status GetModuleInfo(grpc::ServerContext *context, const ModuleManagerProto::Empty *, ModuleManagerProto::ModuleInfos *moduleInfos) override;
  // 启动模块
  grpc::Status StartModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) override;
  // 停止模块
  grpc::Status StopModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) override;
  // 上传模块
  grpc::Status UploadModule(grpc::ServerContext *context, const ModuleManagerProto::Empty *, ModuleManagerProto::Empty *) override;
  // 删除模块
  grpc::Status DeleteModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) override;
  // 获取模块运行时信息
  grpc::Status GetRunningModuleInfo(grpc::ServerContext *context, const ModuleManagerProto::Empty *, ModuleManagerProto::ModuleRunningInfos *moduleRunningInfos) override;
  // 保存启动模块信息
  grpc::Status SaveModuleStartConfig(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfos *moduleInfos, ModuleManagerProto::Empty *) override;

  void SetPlatformAccessControl(std::shared_ptr<PlatformAccessControl> control);
  // 读取脱敏平台配置与连接状态，不产生连接或保存副作用。
  grpc::Status GetPlatformAccessConfig(grpc::ServerContext *, const ModuleManagerProto::Empty *, ModuleManagerProto::PlatformAccessStatus *) override;
  // 校验修订、持久保存并切换连接功能，不记录请求中的令牌。
  grpc::Status SetPlatformAccessConfig(grpc::ServerContext *, const ModuleManagerProto::SetPlatformAccessConfigRequest *, ModuleManagerProto::PlatformAccessStatus *) override;

private:
  ModuleManager *moduleManager_{nullptr};
  std::mutex platformAccessMutex_;
  std::shared_ptr<PlatformAccessControl> platformAccess_;
};
}  // namespace ModuleManager