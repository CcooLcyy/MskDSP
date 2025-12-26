#include "ModuleManagerGrpcService.h"

#include <grpcpp/support/status.h>

#include "ModuleManager.h"
#include "ModuleManager.pb.h"

namespace ModuleManager {
void ModuleManagerServiceImpl::getModuleManager(ModuleManager *moduleManager) {
  moduleManager_ = moduleManager;
}
grpc::Status ModuleManagerServiceImpl::GetModuleInfo(grpc::ServerContext *context, const ModuleManagerProto::Empty *, ModuleManagerProto::ModuleInfos *moduleInfos) {
  moduleInfos->CopyFrom(moduleManager_->getModuleInfos());
  return grpc::Status::OK;
}
grpc::Status ModuleManagerServiceImpl::StartModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) {
  moduleManager_->loadModule(*moduleInfo);
  return grpc::Status::OK;
}
grpc::Status ModuleManagerServiceImpl::StopModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) {
  moduleManager_->unloadModule(*moduleInfo);
  return grpc::Status::OK;
}
grpc::Status ModuleManagerServiceImpl::UploadModule(grpc::ServerContext *context, const ModuleManagerProto::Empty *, ModuleManagerProto::Empty *) {
  return grpc::Status::OK;
}
grpc::Status ModuleManagerServiceImpl::DeleteModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) {
  return grpc::Status::OK;
}
grpc::Status ModuleManagerServiceImpl::GetRunningModuleInfo(grpc::ServerContext *context, const ModuleManagerProto::Empty *, ModuleManagerProto::ModuleRunningInfos *moduleRunningInfos) {
  moduleRunningInfos->CopyFrom(moduleManager_->getModuleRunningInfos());
  return grpc::Status::OK;
}
}  // namespace ModuleManager