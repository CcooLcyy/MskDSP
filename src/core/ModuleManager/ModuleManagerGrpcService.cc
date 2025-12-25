#include "ModuleManagerGrpcService.h"

#include <grpcpp/support/status.h>

#include "ModuleManager.pb.h"

namespace ModuleManager {
grpc::Status ServiceImpl::GetModuleInfo(grpc::ServerContext *context, const ModuleManagerProto::Empty, ModuleManagerProto::ModuleInfos *moduleInfos) {
  *moduleInfos = moduleManager_->getModuleInfo();
  return grpc::Status::OK;
}
grpc::Status ServiceImpl::StartModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo moduleInfo, ModuleManagerProto::Empty *) {}
grpc::Status ServiceImpl::StopModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo moduleInfo, ModuleManagerProto::Empty *) {}
grpc::Status ServiceImpl::UploadModule(grpc::ServerContext *context, const ModuleManagerProto::Empty, ModuleManagerProto::Empty *) {}
grpc::Status ServiceImpl::DeleteModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo moduleInfo, ModuleManagerProto::Empty *) {}
}  // namespace ModuleManager