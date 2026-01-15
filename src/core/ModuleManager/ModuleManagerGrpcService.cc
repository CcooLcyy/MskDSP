#include "ModuleManagerGrpcService.h"

#include <grpcpp/support/status.h>

#include "Logger.h"
#include "ModuleManager.h"
#include "ModuleManager.pb.h"

namespace ModuleManager {
namespace {
grpc::StatusCode toGrpcStatusCode(ModuleOpError error) {
  switch (error) {
    case ModuleOpError::kOk:
      return grpc::StatusCode::OK;
    case ModuleOpError::kNotFound:
      return grpc::StatusCode::NOT_FOUND;
    case ModuleOpError::kInvalidArgument:
      return grpc::StatusCode::INVALID_ARGUMENT;
    case ModuleOpError::kFailedPrecondition:
      return grpc::StatusCode::FAILED_PRECONDITION;
    case ModuleOpError::kInternal:
    default:
      return grpc::StatusCode::INTERNAL;
  }
}
}  // namespace

void ModuleManagerServiceImpl::getModuleManager(ModuleManager *moduleManager) {
  moduleManager_ = moduleManager;
}
grpc::Status ModuleManagerServiceImpl::GetModuleInfo(grpc::ServerContext *context, const ModuleManagerProto::Empty *, ModuleManagerProto::ModuleInfos *moduleInfos) {
  moduleInfos->CopyFrom(moduleManager_->getModuleInfos());
  return grpc::Status::OK;
}
grpc::Status ModuleManagerServiceImpl::StartModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) {
  LOG_INFO("启动模块: {}", moduleInfo->module_name());
  auto result = moduleManager_->loadModule(*moduleInfo);
  if (!result.ok()) {
    LOG_ERROR("启动模块 {} 失败: {}", moduleInfo->module_name(), result.message);
    return grpc::Status(toGrpcStatusCode(result.error), result.message);
  }
  return grpc::Status::OK;
}
grpc::Status ModuleManagerServiceImpl::StopModule(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfo *moduleInfo, ModuleManagerProto::Empty *) {
  LOG_INFO("停止模块: {}", moduleInfo->module_name());
  auto result = moduleManager_->unloadModule(*moduleInfo);
  if (!result.ok()) {
    LOG_ERROR("停止模块 {} 失败: {}", moduleInfo->module_name(), result.message);
    return grpc::Status(toGrpcStatusCode(result.error), result.message);
  }
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
grpc::Status ModuleManagerServiceImpl::SaveModuleStartConfig(grpc::ServerContext *context, const ModuleManagerProto::ModuleInfos *moduleInfos, ModuleManagerProto::Empty *) {
  moduleManager_->saveModuleStartConfig(*moduleInfos);
  return grpc::Status::OK;
}
}  // namespace ModuleManager
