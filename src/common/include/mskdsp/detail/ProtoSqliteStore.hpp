#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>

#include <grpcpp/support/status.h>

#include "mskdsp/ConfigDatabase.h"

namespace mskdsp::detail {
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
      trace("SQLite 保存前配置校验失败: 模块=" + moduleName_ + ", 配置项=" + configKey_ +
            ", 原因=" + status.error_message());
      return status;
    }
    std::string data;
    if (!normalized.SerializeToString(&data)) {
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
        return grpc::Status(grpc::StatusCode::INTERNAL, "SQLite 配置 protobuf 解析失败");
      }
      parsed = normalize(std::move(parsed));
      status = validate_(parsed);
      if (!status.ok()) {
        return status;
      }
      *out = std::move(parsed);
      return grpc::Status::OK;
    }

    out->Clear();
    trace("SQLite 配置项不存在，返回空配置: 模块=" + moduleName_ + ", 配置项=" + configKey_);
    return grpc::Status::OK;
  }

  const std::filesystem::path& databasePath() const { return dbPath_; }

private:
  void trace(const std::string& message) const {
    if (trace_) {
      trace_(message);
    }
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
