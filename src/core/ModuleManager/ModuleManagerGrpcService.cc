#include "ModuleManagerGrpcService.h"

#include <grpcpp/support/status.h>

#include <memory>

#include "ModuleManager.h"
#include "ModuleManager.pb.h"

namespace ModuleManager {
void ServiceImpl::getModuleManager(ModuleManager *moduleManager) {
  moduleManager_ = std::shared_ptr<ModuleManager>(moduleManager);
}
grpc::Status ServiceImpl::GetModuleInfo(grpc::ServerContext *context, const ModuleManagerProto::Empty *, ModuleManagerProto::ModuleInfos *moduleInfos) {
  moduleInfos->CopyFrom(moduleManager_->getModuleInfos());
  return grpc::Status::OK;
}
grpc::Status ServiceImpl::StartModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) {
  moduleManager_->loadModule(*moduleInfo);
  return grpc::Status::OK;
}
grpc::Status ServiceImpl::StopModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) {
  moduleManager_->unloadModule(*moduleInfo);
  return grpc::Status::OK;
}
grpc::Status ServiceImpl::UploadModule(grpc::ServerContext *context, const ModuleManagerProto::Empty *, ModuleManagerProto::Empty *) {}
grpc::Status ServiceImpl::DeleteModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) {}
}  // namespace ModuleManager