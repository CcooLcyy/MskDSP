#include "ModbusTCPLinkStore.h"
#include <unordered_set>
#include <utility>
#include "Logger.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"
namespace ModbusTCP {
namespace {
grpc::Status validate(const ModbusTCPProto::LinksConfig& c) {
  std::unordered_set<std::string> names;
  for (const auto& item : c.links()) {
    if (!item.has_config() || item.config().conn_name().empty() || item.conn_id() == 0) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "ModbusTCP 持久化链路字段不完整");
    if (!names.insert(item.config().conn_name()).second) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "ModbusTCP 持久化链路存在重复 conn_name");
  }
  return grpc::Status::OK;
}
void trace(const std::string& message) { LOG_INFO("{}", message); }
}
LinkStore::LinkStore(std::filesystem::path path) : path_(std::move(path)) {}
grpc::Status LinkStore::Save(const ModbusTCPProto::LinksConfig& c) { mskdsp::detail::ProtoSqliteStore<ModbusTCPProto::LinksConfig> s(path_, "ModbusTCP", "links", "ModbusTCPProto.LinksConfig", validate, trace); return s.Save(c); }
grpc::Status LinkStore::Load(ModbusTCPProto::LinksConfig* out) { mskdsp::detail::ProtoSqliteStore<ModbusTCPProto::LinksConfig> s(path_, "ModbusTCP", "links", "ModbusTCPProto.LinksConfig", validate, trace); return s.Load(out); }
}
