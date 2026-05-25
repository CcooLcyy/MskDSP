#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <grpcpp/support/status.h>

namespace mskdsp::detail {
template <typename ProtoT>
class ProtoFileStore {
public:
  using ValidateFn = grpc::Status (*)(const ProtoT&);
  using TraceFn = std::function<void(const std::string&)>;

  ProtoFileStore(std::filesystem::path path, ValidateFn validate, TraceFn trace = {}) :
    path_(std::move(path)), validate_(validate), trace_(std::move(trace)) {}

  grpc::Status Save(const ProtoT& config) const {
    auto status = validate(config);
    if (!status.ok()) {
      trace("保存前配置校验失败: 原因=" + status.error_message() + ", " + allFileStates(-1));
      return status;
    }

    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) {
      trace("创建配置目录失败: 目录=" + path_.parent_path().string() + ", 原因=" + ec.message());
      return grpc::Status(grpc::StatusCode::INTERNAL, "创建配置目录失败");
    }

    std::string data;
    if (!config.SerializeToString(&data)) {
      trace("序列化 protobuf 失败: " + allFileStates(-1));
      return grpc::Status(grpc::StatusCode::INTERNAL, "序列化 protobuf 失败");
    }
    trace("保存开始: " + allFileStates(static_cast<int64_t>(data.size())));

    const auto tmp = tmpPath();
    trace("准备写入临时文件: 临时文件路径=" + tmp.string() + ", 写入字节数=" + std::to_string(data.size()));
    status = writeStringToFile(tmp, data);
    if (!status.ok()) {
      trace("写入临时文件失败: 原因=" + status.error_message() + ", " + allFileStates(static_cast<int64_t>(data.size())));
      std::error_code rmEc;
      std::filesystem::remove(tmp, rmEc);
      return status;
    }
    trace("临时文件写入完成: " + allFileStates(static_cast<int64_t>(data.size())));

    ProtoT verify;
    trace("准备解析校验临时文件: 临时文件路径=" + tmp.string());
    status = parseAndValidate(tmp, &verify);
    if (!status.ok()) {
      trace("临时文件解析校验失败: 原因=" + status.error_message() + ", " + allFileStates(static_cast<int64_t>(data.size())));
      std::error_code rmEc;
      std::filesystem::remove(tmp, rmEc);
      return status;
    }
    trace("临时文件解析校验成功: " + allFileStates(static_cast<int64_t>(data.size())));

    if (std::filesystem::exists(path_)) {
      ProtoT current;
      trace("主文件存在，准备解析校验当前主文件: 主文件路径=" + path_.string());
      auto curStatus = parseAndValidate(path_, &current);
      if (curStatus.ok()) {
        trace("当前主文件解析校验成功，准备将主文件轮转为备份: 主文件路径=" + path_.string() +
              ", 备份文件路径=" + backupPath().string());
        auto bakStatus = replaceFile(path_, backupPath());
        if (!bakStatus.ok()) {
          trace("主文件轮转为备份失败: 原因=" + bakStatus.error_message() + ", " + allFileStates(static_cast<int64_t>(data.size())));
          std::error_code rmEc;
          std::filesystem::remove(tmp, rmEc);
          return bakStatus;
        }
        trace("主文件轮转为备份完成: " + allFileStates(static_cast<int64_t>(data.size())));
      } else {
        trace("当前主文件解析校验失败，准备隔离主文件: 原因=" + curStatus.error_message() + ", " +
              allFileStates(static_cast<int64_t>(data.size())));
        (void)isolateCorruptFile(path_);
        trace("当前主文件隔离流程结束: " + allFileStates(static_cast<int64_t>(data.size())));
      }
    }

    trace("准备将临时文件替换为主文件: 临时文件路径=" + tmp.string() + ", 主文件路径=" + path_.string());
    status = replaceFile(tmp, path_);
    if (!status.ok()) {
      trace("临时文件替换主文件失败: 原因=" + status.error_message() + ", " + allFileStates(static_cast<int64_t>(data.size())));
      return status;
    }
    trace("临时文件替换主文件完成: " + allFileStates(static_cast<int64_t>(data.size())));
    trace("保存完成: " + allFileStates(static_cast<int64_t>(data.size())));
    return grpc::Status::OK;
  }

  grpc::Status Load(ProtoT* out) const {
    if (out == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
    }

    std::error_code ec;
    trace("加载开始: " + allFileStates(-1));
    if (!std::filesystem::exists(path_, ec) && !std::filesystem::exists(backupPath(), ec)) {
      out->Clear();
      trace("主文件和备份文件均不存在，返回空配置: " + allFileStates(-1));
      return grpc::Status::OK;
    }

    bool mainExists = std::filesystem::exists(path_, ec);
    bool backupExists = std::filesystem::exists(backupPath(), ec);
    grpc::Status mainStatus;
    grpc::Status backupStatus;
    if (mainExists && backupExists) {
      const auto mainState = inspectFileState(path_);
      const auto backupState = inspectFileState(backupPath());
      if (mainState.exists == "true" && mainState.size == "0" &&
          backupState.exists == "true" && backupState.size != "0" && backupState.size != "未知") {
        trace("告警: 主文件为空但备份文件非空，加载流程会优先尝试主文件；如果空 protobuf 解析成功，将返回空配置而不会恢复备份: " +
              allFileStates(-1));
      }
    }

    if (mainExists) {
      ProtoT mainCfg;
      trace("准备解析校验主文件: 主文件路径=" + path_.string());
      mainStatus = parseAndValidate(path_, &mainCfg);
      if (mainStatus.ok()) {
        *out = std::move(mainCfg);
        trace("主文件解析校验成功，使用主文件配置: " + allFileStates(-1));
        return grpc::Status::OK;
      }
      trace("主文件解析校验失败: 原因=" + mainStatus.error_message() + ", " + allFileStates(-1));
    }

    if (backupExists) {
      ProtoT bakCfg;
      trace("准备解析校验备份文件: 备份文件路径=" + backupPath().string());
      backupStatus = parseAndValidate(backupPath(), &bakCfg);
      if (backupStatus.ok()) {
        trace("备份文件解析校验成功，准备用备份恢复主文件: " + allFileStates(-1));
        (void)Save(bakCfg);
        *out = std::move(bakCfg);
        trace("备份文件恢复主文件流程结束，使用备份配置: " + allFileStates(-1));
        return grpc::Status::OK;
      }
      trace("备份文件解析校验失败: 原因=" + backupStatus.error_message() + ", " + allFileStates(-1));
    }

    if (mainExists) {
      trace("主文件不可用，准备隔离主文件: 主文件路径=" + path_.string());
      (void)isolateCorruptFile(path_);
      trace("主文件隔离流程结束: " + allFileStates(-1));
    }
    if (backupExists) {
      trace("备份文件不可用，准备隔离备份文件: 备份文件路径=" + backupPath().string());
      (void)isolateCorruptFile(backupPath());
      trace("备份文件隔离流程结束: " + allFileStates(-1));
    }
    trace("加载失败: " + buildLoadFailureMessage(mainExists, mainStatus, backupExists, backupStatus) + ", " + allFileStates(-1));
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        buildLoadFailureMessage(mainExists, mainStatus, backupExists, backupStatus));
  }

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path backupPath() const { return std::filesystem::path(path_.string() + ".bak"); }
  std::filesystem::path tmpPath() const { return std::filesystem::path(path_.string() + ".tmp"); }

private:
  struct FileState {
    std::string path;
    std::string exists{"未知"};
    std::string size{"未知"};
    std::string writeTimeTicks{"未知"};
    std::string error;
  };

  void trace(const std::string& message) const {
    if (trace_) {
      trace_(message);
    }
  }

  static void appendFileStateError(FileState* state, const char* action, const std::error_code& ec) {
    if (!state->error.empty()) {
      state->error += "; ";
    }
    state->error += action;
    state->error += "失败: ";
    state->error += ec.message();
  }

  static FileState inspectFileState(const std::filesystem::path& path) {
    FileState state;
    state.path = path.string();
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
      appendFileStateError(&state, "检查存在性", ec);
      return state;
    }
    state.exists = exists ? "true" : "false";
    if (!exists) {
      return state;
    }

    ec.clear();
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
      appendFileStateError(&state, "获取大小", ec);
    } else {
      state.size = std::to_string(size);
    }

    ec.clear();
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) {
      appendFileStateError(&state, "获取修改时间", ec);
    } else {
      state.writeTimeTicks = std::to_string(mtime.time_since_epoch().count());
    }
    return state;
  }

  static std::string formatFileState(const char* label, const FileState& state) {
    std::ostringstream oss;
    oss << label << "路径=" << state.path
        << ", " << label << "存在=" << state.exists
        << ", " << label << "大小=" << state.size
        << ", " << label << "修改时间ticks=" << state.writeTimeTicks
        << ", " << label << "状态错误=" << state.error;
    return oss.str();
  }

  std::string allFileStates(int64_t serializedBytes) const {
    std::ostringstream oss;
    if (serializedBytes >= 0) {
      oss << "序列化字节数=" << serializedBytes << ", ";
    }
    oss << formatFileState("主文件", inspectFileState(path_)) << ", "
        << formatFileState("备份文件", inspectFileState(backupPath())) << ", "
        << formatFileState("临时文件", inspectFileState(tmpPath()));
    return oss.str();
  }

  grpc::Status validate(const ProtoT& config) const {
    if (validate_ == nullptr) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "validate 为空");
    }
    return validate_(config);
  }

  static grpc::Status readFileToString(const std::filesystem::path& path, std::string* out) {
    if (out == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "打开文件失败");
    }
    std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (!ifs.good() && !ifs.eof()) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "读取文件失败");
    }
    *out = std::move(data);
    return grpc::Status::OK;
  }

  static grpc::Status writeStringToFile(const std::filesystem::path& path, const std::string& data) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "打开文件用于写入失败");
    }
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!ofs.good()) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "写入文件失败");
    }
    ofs.flush();
    if (!ofs.good()) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "刷新文件失败");
    }
    return grpc::Status::OK;
  }

  grpc::Status parseAndValidate(const std::filesystem::path& path, ProtoT* out) const {
    if (out == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
    }
    std::string data;
    auto status = readFileToString(path, &data);
    if (!status.ok()) {
      return status;
    }
    ProtoT cfg;
    if (!cfg.ParseFromString(data)) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "解析 protobuf 失败");
    }
    status = validate(cfg);
    if (!status.ok()) {
      return status;
    }
    *out = std::move(cfg);
    return grpc::Status::OK;
  }

  static std::string buildLoadFailureMessage(bool mainExists, const grpc::Status& mainStatus,
                                             bool backupExists, const grpc::Status& backupStatus) {
    std::ostringstream oss;
    oss << "配置主文件和备份文件均不可用";
    if (mainExists) {
      oss << "，主文件错误=" << mainStatus.error_message();
    } else {
      oss << "，主文件不存在";
    }
    if (backupExists) {
      oss << "，备份文件错误=" << backupStatus.error_message();
    } else {
      oss << "，备份文件不存在";
    }
    return oss.str();
  }

  static int64_t nowMs() {
    auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return now.time_since_epoch().count();
  }

  static grpc::Status replaceFile(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (!ec) {
      return grpc::Status::OK;
    }

    if (ec == std::errc::file_exists || ec == std::errc::permission_denied) {
      std::error_code rmEc;
      std::filesystem::remove(to, rmEc);
      std::filesystem::rename(from, to, ec);
      if (!ec) {
        return grpc::Status::OK;
      }
    }

    return grpc::Status(grpc::StatusCode::INTERNAL, "重命名文件失败");
  }

  static grpc::Status isolateCorruptFile(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
      return grpc::Status::OK;
    }
    auto corruptPath = std::filesystem::path(path.string() + ".corrupt." + std::to_string(nowMs()));
    auto status = replaceFile(path, corruptPath);
    if (!status.ok()) {
      return status;
    }
    return grpc::Status::OK;
  }

  std::filesystem::path path_;
  ValidateFn validate_{nullptr};
  TraceFn trace_;
};
}  // namespace mskdsp::detail
