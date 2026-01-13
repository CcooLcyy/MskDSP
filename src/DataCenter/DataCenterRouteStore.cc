#include "DataCenterRouteStore.h"

#include "ProtoFileStore.hpp"

namespace DataCenter {
namespace {
grpc::Status validateRoutesConfig(const DataCenterProto::RoutesConfig& config) {
  for (const auto& route : config.routes()) {
    if (route.src().conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "routes contains src conn_id=0");
    }
    if (route.src().tag().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "routes contains empty src tag");
    }
    if (route.dst().conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "routes contains dst conn_id=0");
    }
    if (route.dst().tag().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "routes contains empty dst tag");
    }
  }
  return grpc::Status::OK;
}
}  // namespace

DataCenterRouteStore::DataCenterRouteStore(std::filesystem::path routesPath) :
  routesPath_(std::move(routesPath)) {}

grpc::Status DataCenterRouteStore::Save(const DataCenterProto::RoutesConfig& config) {
  detail::ProtoFileStore<DataCenterProto::RoutesConfig> store(routesPath_, validateRoutesConfig);
  return store.Save(config);
}

grpc::Status DataCenterRouteStore::Load(DataCenterProto::RoutesConfig* out) {
  detail::ProtoFileStore<DataCenterProto::RoutesConfig> store(routesPath_, validateRoutesConfig);
  return store.Load(out);
}

std::filesystem::path DataCenterRouteStore::routesPath() const {
  return routesPath_;
}

std::filesystem::path DataCenterRouteStore::backupPath() const {
  detail::ProtoFileStore<DataCenterProto::RoutesConfig> store(routesPath_, validateRoutesConfig);
  return store.backupPath();
}

std::filesystem::path DataCenterRouteStore::tmpPath() const {
  detail::ProtoFileStore<DataCenterProto::RoutesConfig> store(routesPath_, validateRoutesConfig);
  return store.tmpPath();
}
}  // namespace DataCenter
