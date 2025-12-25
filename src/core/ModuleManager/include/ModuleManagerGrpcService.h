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
  grpc::Status GetModuleInfo(grpc::ServerContext *context, const ModuleManagerProto::Empty, ModuleManagerProto::ModuleInfos *moduleInfos);
  grpc::Status StartModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo moduleInfo, ModuleManagerProto::Empty *);
  grpc::Status StopModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo moduleInfo, ModuleManagerProto::Empty *);
  grpc::Status UploadModule(grpc::ServerContext *context, const ModuleManagerProto::Empty, ModuleManagerProto::Empty *);
  grpc::Status DeleteModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo moduleInfo, ModuleManagerProto::Empty *);

private:
  std::shared_ptr<ModuleManager> moduleManager_;
};
}  // namespace ModuleManager