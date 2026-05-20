#include "DataCenterRouteStore.h"

#include "mskdsp/detail/ProtoFileStore.hpp"

namespace DataCenter {
namespace {
grpc::Status validateRoutesConfig(const DataCenterProto::RoutesConfig& config) {
  for (const auto& route : config.routes()) {
    if (route.src().tag().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "routes 包含空 src tag");
    }
    if (route.dst().tag().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "routes 包含空 dst tag");
    }
    if (route.src().module_name().empty() != route.src().conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "routes 包含不完整的 src 稳定连接主键");
    }
    const bool srcHasStableKey = !route.src().module_name().empty() && !route.src().conn_name().empty();
    if (!srcHasStableKey && route.src().conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "routes 包含无法识别的 src 端点");
    }
    if (route.dst().module_name().empty() != route.dst().conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "routes 包含不完整的 dst 稳定连接主键");
    }
    const bool dstHasStableKey = !route.dst().module_name().empty() && !route.dst().conn_name().empty();
    if (!dstHasStableKey && route.dst().conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "routes 包含无法识别的 dst 端点");
    }
  }
  return grpc::Status::OK;
}
}  // namespace

DataCenterRouteStore::DataCenterRouteStore(std::filesystem::path routesPath) :
  routesPath_(std::move(routesPath)) {}

grpc::Status DataCenterRouteStore::Save(const DataCenterProto::RoutesConfig& config) {
  mskdsp::detail::ProtoFileStore<DataCenterProto::RoutesConfig> store(routesPath_,
                                                                      validateRoutesConfig);
  return store.Save(config);
}

grpc::Status DataCenterRouteStore::Load(DataCenterProto::RoutesConfig* out) {
  mskdsp::detail::ProtoFileStore<DataCenterProto::RoutesConfig> store(routesPath_,
                                                                      validateRoutesConfig);
  return store.Load(out);
}

std::filesystem::path DataCenterRouteStore::routesPath() const {
  return routesPath_;
}

std::filesystem::path DataCenterRouteStore::backupPath() const {
  mskdsp::detail::ProtoFileStore<DataCenterProto::RoutesConfig> store(routesPath_,
                                                                      validateRoutesConfig);
  return store.backupPath();
}

std::filesystem::path DataCenterRouteStore::tmpPath() const {
  mskdsp::detail::ProtoFileStore<DataCenterProto::RoutesConfig> store(routesPath_,
                                                                      validateRoutesConfig);
  return store.tmpPath();
}
}  // namespace DataCenter
