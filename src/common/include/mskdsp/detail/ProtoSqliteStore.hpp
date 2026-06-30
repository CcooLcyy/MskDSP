#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <utility>

#include <grpcpp/support/status.h>

#include "mskdsp/ConfigDatabase.h"

namespace mskdsp::detail {
inline std::string ConfigStorePathFields(const std::filesystem::path& path) {
  std::error_code ec;
  const auto absPath = std::filesystem::absolute(path, ec);
  std::string out = "db_path=" + path.string();
  if (ec) {
    out += ", abs_db_path=<解析失败:" + ec.message() + ">";
  } else {
    out += ", abs_db_path=" + absPath.string();
  }
  return out;
}

template <typename ProtoT>
class ProtoSqliteStore {
public:
  using ValidateFn = grpc::Status (*)(const ProtoT&);
  using TraceFn = std::function<void(const std::string&)>;
  using NormalizeFn = std::function<ProtoT(ProtoT)>;

  ProtoSqliteStore(std::filesystem::path configDbPath,
                   std::string moduleName,
                   std::string configKey,
                   std::string protoType,
                   ValidateFn validate,
                   TraceFn trace = {},
                   NormalizeFn normalize = {}) :
    dbPath_(std::move(configDbPath)),
    moduleName_(std::move(moduleName)),
    configKey_(std::move(configKey)),
    protoType_(std::move(protoType)),
    validate_(validate),
    trace_(std::move(trace)),
    normalize_(std::move(normalize)) {}

  grpc::Status Save(const ProtoT& config) const {
    auto normalized = normalize(config);
    auto status = validate_(normalized);
    if (!status.ok()) {
      trace("SQLite 保存前配置校验失败: " + identityFields() +
            ", 原因=" + status.error_message());
      return status;
    }
    std::string data;
    if (!normalized.SerializeToString(&data)) {
      trace("SQLite 配置 protobuf 序列化失败: " + identityFields());
      return grpc::Status(grpc::StatusCode::INTERNAL, "序列化 protobuf 失败");
    }
    ConfigDatabase db(dbPath_);
    return db.SaveBlob(moduleName_, configKey_, protoType_, data, 1, trace_);
  }

  grpc::Status Load(ProtoT* out) const {
    if (out == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
    }
    ConfigDatabase db(dbPath_);
    std::string data;
    bool found = false;
    auto status = db.LoadBlob(moduleName_, configKey_, &data, &found, trace_);
    if (!status.ok()) {
      return status;
    }
    if (found) {
      ProtoT parsed;
      if (!parsed.ParseFromString(data)) {
        trace("SQLite 配置 protobuf 解析失败: " + identityFields() +
              ", found=true, payload_size=" + std::to_string(data.size()));
        return grpc::Status(grpc::StatusCode::INTERNAL, "SQLite 配置 protobuf 解析失败");
      }
      parsed = normalize(std::move(parsed));
      status = validate_(parsed);
      if (!status.ok()) {
        trace("SQLite 加载后配置校验失败: " + identityFields() +
              ", found=true, payload_size=" + std::to_string(data.size()) +
              ", 原因=" + status.error_message());
        return status;
      }
      *out = std::move(parsed);
      return grpc::Status::OK;
    }

    out->Clear();
    trace("SQLite 配置项不存在，返回空配置: " + identityFields() +
          ", found=false, payload_size=0");
    return grpc::Status::OK;
  }

  const std::filesystem::path& databasePath() const { return dbPath_; }

private:
  void trace(const std::string& message) const {
    if (trace_) {
      trace_(message);
    }
  }

  std::string identityFields() const {
    return ConfigStorePathFields(dbPath_) + ", module_name=" + moduleName_ + ", config_key=" + configKey_;
  }

  ProtoT normalize(ProtoT config) const {
    if (normalize_) {
      return normalize_(std::move(config));
    }
    return config;
  }

  std::filesystem::path dbPath_;
  std::string moduleName_;
  std::string configKey_;
  std::string protoType_;
  ValidateFn validate_;
  TraceFn trace_;
  NormalizeFn normalize_;
};
}  // namespace mskdsp::detail
