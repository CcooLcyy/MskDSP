#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include <grpcpp/support/status.h>

namespace DataCenter::detail {
template <typename ProtoT>
class ProtoFileStore {
public:
  using ValidateFn = grpc::Status (*)(const ProtoT&);

  ProtoFileStore(std::filesystem::path path, ValidateFn validate) :
    path_(std::move(path)), validate_(validate) {}

  grpc::Status Save(const ProtoT& config) const {
    auto status = validate(config);
    if (!status.ok()) {
      return status;
    }

    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "failed to create config directory");
    }

    std::string data;
    if (!config.SerializeToString(&data)) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "failed to serialize protobuf");
    }

    const auto tmp = tmpPath();
    status = writeStringToFile(tmp, data);
    if (!status.ok()) {
      std::error_code rmEc;
      std::filesystem::remove(tmp, rmEc);
      return status;
    }

    ProtoT verify;
    status = parseAndValidate(tmp, &verify);
    if (!status.ok()) {
      std::error_code rmEc;
      std::filesystem::remove(tmp, rmEc);
      return status;
    }

    if (std::filesystem::exists(path_)) {
      ProtoT current;
      auto curStatus = parseAndValidate(path_, &current);
      if (curStatus.ok()) {
        auto bakStatus = replaceFile(path_, backupPath());
        if (!bakStatus.ok()) {
          std::error_code rmEc;
          std::filesystem::remove(tmp, rmEc);
          return bakStatus;
        }
      } else {
        (void)isolateCorruptFile(path_);
      }
    }

    status = replaceFile(tmp, path_);
    if (!status.ok()) {
      return status;
    }
    return grpc::Status::OK;
  }

  grpc::Status Load(ProtoT* out) const {
    if (out == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
    }

    std::error_code ec;
    if (!std::filesystem::exists(path_, ec) && !std::filesystem::exists(backupPath(), ec)) {
      out->Clear();
      return grpc::Status::OK;
    }

    if (std::filesystem::exists(path_, ec)) {
      ProtoT mainCfg;
      auto status = parseAndValidate(path_, &mainCfg);
      if (status.ok()) {
        *out = std::move(mainCfg);
        return grpc::Status::OK;
      }
    }

    if (std::filesystem::exists(backupPath(), ec)) {
      ProtoT bakCfg;
      auto status = parseAndValidate(backupPath(), &bakCfg);
      if (status.ok()) {
        (void)Save(bakCfg);
        *out = std::move(bakCfg);
        return grpc::Status::OK;
      }
    }

    (void)isolateCorruptFile(path_);
    out->Clear();
    return grpc::Status::OK;
  }

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path backupPath() const { return std::filesystem::path(path_.string() + ".bak"); }
  std::filesystem::path tmpPath() const { return std::filesystem::path(path_.string() + ".tmp"); }

private:
  grpc::Status validate(const ProtoT& config) const {
    if (validate_ == nullptr) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "validate is null");
    }
    return validate_(config);
  }

  static grpc::Status readFileToString(const std::filesystem::path& path, std::string* out) {
    if (out == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "failed to open file");
    }
    std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (!ifs.good() && !ifs.eof()) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "failed to read file");
    }
    *out = std::move(data);
    return grpc::Status::OK;
  }

  static grpc::Status writeStringToFile(const std::filesystem::path& path, const std::string& data) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "failed to open file for write");
    }
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!ofs.good()) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "failed to write file");
    }
    ofs.flush();
    if (!ofs.good()) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "failed to flush file");
    }
    return grpc::Status::OK;
  }

  grpc::Status parseAndValidate(const std::filesystem::path& path, ProtoT* out) const {
    if (out == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
    }
    std::string data;
    auto status = readFileToString(path, &data);
    if (!status.ok()) {
      return status;
    }
    ProtoT cfg;
    if (!cfg.ParseFromString(data)) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "failed to parse protobuf");
    }
    status = validate(cfg);
    if (!status.ok()) {
      return status;
    }
    *out = std::move(cfg);
    return grpc::Status::OK;
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

    return grpc::Status(grpc::StatusCode::INTERNAL, "failed to rename file");
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
};
}  // namespace DataCenter::detail

