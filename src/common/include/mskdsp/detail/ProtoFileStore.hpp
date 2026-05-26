#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <grpcpp/support/status.h>
#include <unistd.h>

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
    std::filesystem::create_directories(storageDir(), ec);
    if (ec) {
      trace("创建配置目录失败: 目录=" + storageDir().string() + ", 原因=" + ec.message());
      return grpc::Status(grpc::StatusCode::INTERNAL, "创建配置目录失败");
    }
    status = syncDirectory(storageDir());
    if (!status.ok()) {
      trace("配置目录同步失败: 目录=" + storageDir().string() + ", 原因=" + status.error_message());
      return status;
    }
    trace("配置目录同步完成: 目录=" + storageDir().string());

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
    trace("临时文件写入并 fsync 完成: " + allFileStates(static_cast<int64_t>(data.size())));
    status = syncDirectory(storageDir());
    if (!status.ok()) {
      trace("临时文件创建后同步目录失败: 原因=" + status.error_message() + ", " + allFileStates(static_cast<int64_t>(data.size())));
      std::error_code rmEc;
      std::filesystem::remove(tmp, rmEc);
      return status;
    }
    trace("临时文件创建后同步目录完成: " + allFileStates(static_cast<int64_t>(data.size())));

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
        bakStatus = syncDirectory(storageDir());
        if (!bakStatus.ok()) {
          trace("主文件轮转为备份后同步目录失败: 原因=" + bakStatus.error_message() + ", " + allFileStates(static_cast<int64_t>(data.size())));
          std::error_code rmEc;
          std::filesystem::remove(tmp, rmEc);
          return bakStatus;
        }
        trace("主文件轮转为备份完成: " + allFileStates(static_cast<int64_t>(data.size())));
      } else {
        trace("当前主文件解析校验失败，准备隔离主文件: 原因=" + curStatus.error_message() + ", " +
              allFileStates(static_cast<int64_t>(data.size())));
        auto isolateStatus = isolateCorruptFile(path_);
        if (!isolateStatus.ok()) {
          trace("当前主文件隔离失败: 原因=" + isolateStatus.error_message() + ", " + allFileStates(static_cast<int64_t>(data.size())));
          std::error_code rmEc;
          std::filesystem::remove(tmp, rmEc);
          return isolateStatus;
        }
        trace("当前主文件隔离流程结束: " + allFileStates(static_cast<int64_t>(data.size())));
      }
    }

    trace("准备将临时文件替换为主文件: 临时文件路径=" + tmp.string() + ", 主文件路径=" + path_.string());
    status = replaceFile(tmp, path_);
    if (!status.ok()) {
      trace("临时文件替换主文件失败: 原因=" + status.error_message() + ", " + allFileStates(static_cast<int64_t>(data.size())));
      return status;
    }
    status = syncDirectory(storageDir());
    if (!status.ok()) {
      trace("临时文件替换主文件后同步目录失败: 原因=" + status.error_message() + ", " + allFileStates(static_cast<int64_t>(data.size())));
      return status;
    }
    trace("临时文件替换主文件完成: " + allFileStates(static_cast<int64_t>(data.size())));

    ProtoT finalVerify;
    status = parseAndValidate(path_, &finalVerify);
    if (!status.ok()) {
      trace("保存完成后主文件解析校验失败: 原因=" + status.error_message() + ", " + allFileStates(static_cast<int64_t>(data.size())));
      return status;
    }
    trace("保存完成后主文件解析校验成功: " + allFileStates(static_cast<int64_t>(data.size())));
    trace("保存完成: " + allFileStates(static_cast<int64_t>(data.size())));
    return grpc::Status::OK;
  }

  grpc::Status Load(ProtoT* out) const {
    if (out == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
    }

    std::error_code ec;
    trace("加载开始: " + allFileStates(-1));
    if (!std::filesystem::exists(path_, ec) && !std::filesystem::exists(tmpPath(), ec) &&
        !std::filesystem::exists(backupPath(), ec)) {
      out->Clear();
      trace("主文件、临时文件和备份文件均不存在，返回空配置: " + allFileStates(-1));
      return grpc::Status::OK;
    }

    bool mainExists = std::filesystem::exists(path_, ec);
    bool tmpExists = std::filesystem::exists(tmpPath(), ec);
    bool backupExists = std::filesystem::exists(backupPath(), ec);
    grpc::Status mainStatus;
    grpc::Status tmpStatus;
    grpc::Status backupStatus;
    if (mainExists && backupExists) {
      const auto mainState = inspectFileState(path_);
      const auto backupState = inspectFileState(backupPath());
      if (mainState.exists == "true" && mainState.size == "0" &&
          backupState.exists == "true" && backupState.size != "0" && backupState.size != "未知") {
        trace("告警: 主文件为空但备份文件非空，加载流程会将主文件判为无效后继续尝试临时文件和备份文件: " +
              allFileStates(-1));
      }
    }

    if (mainExists) {
      ProtoT mainCfg;
      trace("准备解析校验主文件: 主文件路径=" + path_.string());
      mainStatus = parseAndValidate(path_, &mainCfg);
      if (mainStatus.ok()) {
        if (tmpExists) {
          ProtoT tmpCfg;
          trace("主文件解析校验成功但临时文件仍存在，准备判断临时文件是否为更新版本: 临时文件路径=" + tmpPath().string());
          tmpStatus = parseAndValidate(tmpPath(), &tmpCfg);
          if (tmpStatus.ok()) {
            bool timeKnown = false;
            const bool tmpNewer = isFileNewerThan(tmpPath(), path_, &timeKnown);
            if (timeKnown && tmpNewer) {
              trace("临时文件有效且修改时间晚于主文件，准备用临时文件恢复主文件: " + allFileStates(-1));
              auto restoreStatus = restoreTmpFileToMain();
              if (!restoreStatus.ok()) {
                trace("临时文件恢复主文件失败，将继续使用已校验的临时文件内存配置: 原因=" + restoreStatus.error_message() +
                      ", " + allFileStates(-1));
              } else {
                trace("临时文件恢复主文件流程结束，使用临时文件配置: " + allFileStates(-1));
              }
              *out = std::move(tmpCfg);
              return grpc::Status::OK;
            }
            trace("临时文件有效但未确认晚于主文件，本次使用主文件并保留临时文件供排查: 时间可判定=" +
                  std::string(timeKnown ? "true" : "false") + ", 临时文件晚于主文件=" +
                  std::string(tmpNewer ? "true" : "false") + ", " + allFileStates(-1));
          } else {
            trace("临时文件解析校验失败但主文件有效，本次使用主文件并保留临时文件供排查: 原因=" + tmpStatus.error_message() +
                  ", " + allFileStates(-1));
          }
        }
        *out = std::move(mainCfg);
        trace("主文件解析校验成功，使用主文件配置: " + allFileStates(-1));
        return grpc::Status::OK;
      }
      trace("主文件解析校验失败: 原因=" + mainStatus.error_message() + ", " + allFileStates(-1));
    }

    if (tmpExists) {
      ProtoT tmpCfg;
      trace("准备解析校验临时文件: 临时文件路径=" + tmpPath().string());
      tmpStatus = parseAndValidate(tmpPath(), &tmpCfg);
      if (tmpStatus.ok()) {
        trace("临时文件解析校验成功，准备用临时文件恢复主文件: " + allFileStates(-1));
        auto restoreStatus = restoreTmpFileToMain();
        if (!restoreStatus.ok()) {
          trace("临时文件恢复主文件失败，将继续使用已校验的临时文件内存配置: 原因=" + restoreStatus.error_message() +
                ", " + allFileStates(-1));
        } else {
          trace("临时文件恢复主文件流程结束，使用临时文件配置: " + allFileStates(-1));
        }
        *out = std::move(tmpCfg);
        return grpc::Status::OK;
      }
      trace("临时文件解析校验失败: 原因=" + tmpStatus.error_message() + ", " + allFileStates(-1));
    }

    if (backupExists) {
      ProtoT bakCfg;
      trace("准备解析校验备份文件: 备份文件路径=" + backupPath().string());
      backupStatus = parseAndValidate(backupPath(), &bakCfg);
      if (backupStatus.ok()) {
        trace("备份文件解析校验成功，准备用备份恢复主文件: " + allFileStates(-1));
        auto restoreStatus = Save(bakCfg);
        if (!restoreStatus.ok()) {
          trace("备份文件恢复主文件失败，将继续使用已校验的备份文件内存配置: 原因=" + restoreStatus.error_message() +
                ", " + allFileStates(-1));
        } else {
          trace("备份文件恢复主文件流程结束，使用备份配置: " + allFileStates(-1));
        }
        *out = std::move(bakCfg);
        return grpc::Status::OK;
      }
      trace("备份文件解析校验失败: 原因=" + backupStatus.error_message() + ", " + allFileStates(-1));
    }

    if (mainExists) {
      trace("主文件不可用，准备隔离主文件: 主文件路径=" + path_.string());
      (void)isolateCorruptFile(path_);
      trace("主文件隔离流程结束: " + allFileStates(-1));
    }
    if (tmpExists) {
      trace("临时文件不可用，准备隔离临时文件: 临时文件路径=" + tmpPath().string());
      (void)isolateCorruptFile(tmpPath());
      trace("临时文件隔离流程结束: " + allFileStates(-1));
    }
    if (backupExists) {
      trace("备份文件不可用，准备隔离备份文件: 备份文件路径=" + backupPath().string());
      (void)isolateCorruptFile(backupPath());
      trace("备份文件隔离流程结束: " + allFileStates(-1));
    }
    trace("加载失败: " + buildLoadFailureMessage(mainExists, mainStatus, tmpExists, tmpStatus, backupExists, backupStatus) +
          ", " + allFileStates(-1));
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        buildLoadFailureMessage(mainExists, mainStatus, tmpExists, tmpStatus, backupExists, backupStatus));
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
    std::string hash{"未知"};
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

    ec.clear();
    auto hash = fileHash(path, ec);
    if (ec) {
      appendFileStateError(&state, "计算hash", ec);
    } else {
      state.hash = hash;
    }
    return state;
  }

  static std::string formatFileState(const char* label, const FileState& state) {
    std::ostringstream oss;
    oss << label << "路径=" << state.path
        << ", " << label << "存在=" << state.exists
        << ", " << label << "大小=" << state.size
        << ", " << label << "修改时间ticks=" << state.writeTimeTicks
        << ", " << label << "hash=" << state.hash
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

  std::filesystem::path storageDir() const {
    auto parent = path_.parent_path();
    if (parent.empty()) {
      return std::filesystem::path(".");
    }
    return parent;
  }

  static std::string errnoMessage(int err) {
    return std::strerror(err);
  }

  static grpc::Status closeFd(int fd, const char* target) {
    if (::close(fd) != 0) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          std::string(target) + "关闭失败: " + errnoMessage(errno));
    }
    return grpc::Status::OK;
  }

  static grpc::Status fsyncFd(int fd, const char* target) {
    while (::fsync(fd) != 0) {
      if (errno == EINTR) {
        continue;
      }
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          std::string(target) + "fsync失败: " + errnoMessage(errno));
    }
    return grpc::Status::OK;
  }

  static grpc::Status syncDirectory(const std::filesystem::path& dir) {
    const auto dirString = dir.empty() ? std::string(".") : dir.string();
    int fd = ::open(dirString.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "打开目录用于fsync失败: 目录=" + dirString + ", 原因=" + errnoMessage(errno));
    }
    auto status = fsyncFd(fd, "目录");
    auto closeStatus = closeFd(fd, "目录");
    if (!status.ok()) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          status.error_message() + ", 目录=" + dirString);
    }
    if (!closeStatus.ok()) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          closeStatus.error_message() + ", 目录=" + dirString);
    }
    return grpc::Status::OK;
  }

  static std::string formatHex(uint64_t value) {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << value;
    return oss.str();
  }

  static std::string fileHash(const std::filesystem::path& path, std::error_code& ec) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
      ec = std::make_error_code(std::errc::no_such_file_or_directory);
      return "未知";
    }

    uint64_t hash = 14695981039346656037ull;
    char buffer[4096];
    while (ifs) {
      ifs.read(buffer, sizeof(buffer));
      const auto count = ifs.gcount();
      for (std::streamsize i = 0; i < count; ++i) {
        hash ^= static_cast<unsigned char>(buffer[i]);
        hash *= 1099511628211ull;
      }
    }
    if (ifs.bad()) {
      ec = std::make_error_code(std::errc::io_error);
      return "未知";
    }
    ec.clear();
    return formatHex(hash);
  }

  static bool isFileNewerThan(const std::filesystem::path& candidate, const std::filesystem::path& baseline,
                              bool* known) {
    if (known != nullptr) {
      *known = false;
    }
    std::error_code ec;
    const auto candidateTime = std::filesystem::last_write_time(candidate, ec);
    if (ec) {
      return false;
    }
    const auto baselineTime = std::filesystem::last_write_time(baseline, ec);
    if (ec) {
      return false;
    }
    if (known != nullptr) {
      *known = true;
    }
    return candidateTime > baselineTime;
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
    const auto pathString = path.string();
    int fd = ::open(pathString.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "打开文件用于写入失败: 路径=" + pathString + ", 原因=" + errnoMessage(errno));
    }

    const char* cursor = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
      const auto written = ::write(fd, cursor, remaining);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        const auto err = errno;
        (void)closeFd(fd, "文件");
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "写入文件失败: 路径=" + pathString + ", 原因=" + errnoMessage(err));
      }
      if (written == 0) {
        (void)closeFd(fd, "文件");
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "写入文件失败: 路径=" + pathString + ", 原因=写入0字节");
      }
      cursor += written;
      remaining -= static_cast<size_t>(written);
    }

    auto status = fsyncFd(fd, "文件");
    auto closeStatus = closeFd(fd, "文件");
    if (!status.ok()) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          status.error_message() + ", 路径=" + pathString);
    }
    if (!closeStatus.ok()) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          closeStatus.error_message() + ", 路径=" + pathString);
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
                                             bool tmpExists, const grpc::Status& tmpStatus,
                                             bool backupExists, const grpc::Status& backupStatus) {
    std::ostringstream oss;
    oss << "配置主文件、临时文件和备份文件均不可用";
    if (mainExists) {
      oss << "，主文件错误=" << mainStatus.error_message();
    } else {
      oss << "，主文件不存在";
    }
    if (tmpExists) {
      oss << "，临时文件错误=" << tmpStatus.error_message();
    } else {
      oss << "，临时文件不存在";
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
    const auto fromString = from.string();
    const auto toString = to.string();
    while (::rename(fromString.c_str(), toString.c_str()) != 0) {
      if (errno == EINTR) {
        continue;
      }
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "重命名文件失败: from=" + fromString + ", to=" + toString +
                              ", 原因=" + errnoMessage(errno));
    }
    return grpc::Status::OK;
  }

  static std::filesystem::path makeCorruptPath(const std::filesystem::path& path) {
    auto corruptPath = std::filesystem::path(path.string() + ".corrupt." + std::to_string(nowMs()));
    for (int i = 1; std::filesystem::exists(corruptPath); ++i) {
      corruptPath = std::filesystem::path(path.string() + ".corrupt." + std::to_string(nowMs()) + "." + std::to_string(i));
    }
    return corruptPath;
  }

  grpc::Status restoreTmpFileToMain() const {
    std::error_code ec;
    if (std::filesystem::exists(path_, ec)) {
      ProtoT current;
      auto currentStatus = parseAndValidate(path_, &current);
      if (currentStatus.ok()) {
        trace("恢复临时文件前当前主文件有效，准备将主文件轮转为备份: 主文件路径=" + path_.string() +
              ", 备份文件路径=" + backupPath().string());
        auto backupStatus = replaceFile(path_, backupPath());
        if (!backupStatus.ok()) {
          return backupStatus;
        }
        backupStatus = syncDirectory(storageDir());
        if (!backupStatus.ok()) {
          return backupStatus;
        }
      } else {
        trace("恢复临时文件前当前主文件无效，准备隔离主文件: 原因=" + currentStatus.error_message() +
              ", 主文件路径=" + path_.string());
        auto status = isolateCorruptFile(path_);
        if (!status.ok()) {
          return status;
        }
      }
    }

    auto status = replaceFile(tmpPath(), path_);
    if (!status.ok()) {
      return status;
    }
    status = syncDirectory(storageDir());
    if (!status.ok()) {
      return status;
    }
    ProtoT verify;
    status = parseAndValidate(path_, &verify);
    if (!status.ok()) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "临时文件恢复主文件后解析校验失败: " + status.error_message());
    }
    return grpc::Status::OK;
  }

  grpc::Status isolateCorruptFile(const std::filesystem::path& path) const {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      return grpc::Status::OK;
    }
    if (ec) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "检查待隔离文件失败: 路径=" + path.string() + ", 原因=" + ec.message());
    }
    auto corruptPath = makeCorruptPath(path);
    auto status = replaceFile(path, corruptPath);
    if (!status.ok()) {
      return status;
    }
    status = syncDirectory(storageDir());
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
