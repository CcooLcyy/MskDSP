#include "PlatformAccessStore.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <boost/json.hpp>
#include <cerrno>
#include <limits>
#include <stdexcept>

namespace mskdsp::platformagent {
namespace {
struct Descriptor {
  int value{-1};
  ~Descriptor() {
    if (value >= 0)
      ::close(value);
  }
};
std::runtime_error FileError() {
  return std::runtime_error("平台接入配置文件操作失败，请检查目录、权限和磁盘");
}
void CheckDestination(int directory, const std::string &name) {
  struct stat info{};
  if (::fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(info.st_mode))
      throw FileError();
  } else if (errno != ENOENT) {
    throw FileError();
  }
}
uint64_t Unsigned(const boost::json::value &value) {
  if (value.is_uint64())
    return value.as_uint64();
  if (value.is_int64() && value.as_int64() >= 0)
    return static_cast<uint64_t>(value.as_int64());
  throw FileError();
}
}  // namespace

std::string ValidatePlatformAccess(const Config &config) {
  if (config.deviceId.size() > 128)
    return "设备ID长度必须不超过128字节";
  for (unsigned char c : config.deviceId) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return "设备ID只能包含字母、数字、点、下划线和连字符";
  }
  if (config.token.size() > 4096)
    return "设备令牌长度必须不超过4096字节";
  for (unsigned char c : config.token)
    if (c < 33 || c > 126)
      return "设备令牌只能包含可见ASCII字符且不能包含空格";
  if (config.enabled && (config.deviceId.empty() || config.token.empty()))
    return "开启平台接入需要设备ID和设备令牌";
  if (config.sshPort == 0)
    return "SSH端口必须为1至65535";
  return {};
}

PlatformAccessStore::PlatformAccessStore(std::filesystem::path path) : path_(std::move(path)) {}

PlatformAccessLoad PlatformAccessStore::Load() const {
  PlatformAccessLoad result;
  result.saved.config.target = kPlatformTarget;
  try {
    const auto parent = path_.parent_path().empty() ? std::filesystem::path(".") : path_.parent_path();
    Descriptor directory{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (directory.value < 0) {
      if (errno == ENOENT)
        return result;
      throw FileError();
    }
    CheckDestination(directory.value, path_.filename().string());
    Descriptor input{::openat(directory.value, path_.filename().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (input.value < 0) {
      if (errno == ENOENT)
        return result;
      throw FileError();
    }
    result.exists = true;
    struct stat info{};
    if (::fstat(input.value, &info) != 0 || !S_ISREG(info.st_mode) ||
        (info.st_mode & 0777) != 0600 || info.st_size > 64 * 1024)
      throw FileError();
    std::string content;
    char buffer[4096];
    while (true) {
      const auto count = ::read(input.value, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0)
        throw FileError();
      if (count == 0)
        break;
      content.append(buffer, static_cast<size_t>(count));
      if (content.size() > 64 * 1024)
        throw FileError();
    }
    const auto value = boost::json::parse(content);
    const auto &object = value.as_object();
    if (Unsigned(object.at("schema_version")) != 1)
      throw FileError();
    auto saved = result.saved;
    saved.revision = Unsigned(object.at("revision"));
    saved.config.enabled = object.at("enabled").as_bool();
    saved.config.sshEnabled = object.at("ssh_enabled").as_bool();
    saved.config.deviceId = std::string(object.at("device_id").as_string());
    saved.config.token = std::string(object.at("device_token").as_string());
    const auto port = Unsigned(object.at("ssh_port"));
    if (port == 0 || port > 65535 || saved.revision == 0)
      throw FileError();
    saved.config.sshPort = static_cast<uint16_t>(port);
    if (!ValidatePlatformAccess(saved.config).empty())
      throw FileError();
    result.saved = std::move(saved);
  } catch (...) {
    result.error = "平台接入配置加载失败，接入已关闭，请重新保存配置";
  }
  return result;
}

PlatformAccessSave PlatformAccessStore::Save(const StoredPlatformAccess &saved) const {
  if (!ValidatePlatformAccess(saved.config).empty() || saved.revision == 0)
    throw FileError();
  const auto parent = path_.parent_path().empty() ? std::filesystem::path(".") : path_.parent_path();
  Descriptor directory{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (directory.value < 0)
    throw FileError();
  const auto destination = path_.filename().string();
  CheckDestination(directory.value, destination);
  boost::json::object object{
      {"schema_version", 1}, {"revision", saved.revision}, {"enabled", saved.config.enabled}, {"device_id", saved.config.deviceId}, {"device_token", saved.config.token}, {"ssh_enabled", saved.config.sshEnabled}, {"ssh_port", saved.config.sshPort}};
  const auto content = boost::json::serialize(object);
  // openat将临时文件和替换固定在同一已打开目录，拒绝跟随目标文件符号链接。
  std::string temporary;
  Descriptor output;
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    static std::atomic_uint64_t sequence{0};
    temporary = ".platform-access-" + std::to_string(::getpid()) + "-" + std::to_string(sequence.fetch_add(1));
    output.value = ::openat(directory.value, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output.value >= 0)
      break;
    if (errno != EEXIST)
      throw FileError();
  }
  if (output.value < 0)
    throw FileError();
  struct Cleanup {
    int directory;
    const std::string &name;
    ~Cleanup() {
      ::unlinkat(directory, name.c_str(), 0);
    }
  } cleanup{directory.value, temporary};
  if (::fchmod(output.value, 0600) != 0)
    throw FileError();
  size_t offset = 0;
  while (offset < content.size()) {
    const auto count = ::write(output.value, content.data() + offset, content.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw FileError();
    offset += static_cast<size_t>(count);
  }
  if (::fsync(output.value) != 0)
    throw FileError();
  CheckDestination(directory.value, destination);
  if (::renameat(directory.value, temporary.c_str(), directory.value, destination.c_str()) != 0)
    throw FileError();
  return ::fsync(directory.value) == 0 ? PlatformAccessSave::Durable : PlatformAccessSave::DurabilityUncertain;
}
}  // namespace mskdsp::platformagent
