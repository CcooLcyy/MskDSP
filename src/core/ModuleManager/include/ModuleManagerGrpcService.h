#pragma once
#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/sync_stream.h>

#include <memory>

#include "ModuleManager.grpc.pb.h"
#include "ModuleManager.h"
#include "ModuleManager.pb.h"

namespace ModuleManager {
class ServiceImpl : public ModuleManagerProto::ModuleManage::Service {
public:
  void getModuleManager(ModuleManager *moduleManager);
  grpc::Status GetModuleInfo(grpc::ServerContext *context, const ModuleManagerProto::Empty *, ModuleManagerProto::ModuleInfos *moduleInfos) override;
  grpc::Status StartModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) override;
  grpc::Status StopModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) override;
  grpc::Status UploadModule(grpc::ServerContext *context, const ModuleManagerProto::Empty *, ModuleManagerProto::Empty *) override;
  grpc::Status DeleteModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) override;

private:
  std::shared_ptr<ModuleManager> moduleManager_;
};
}  // namespace ModuleManager