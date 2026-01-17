#include "COMMock.h"

#include <boost/dll.hpp>

#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

#include "COMMock.pb.h"
#include "COMMockGrpcService.h"
#include "COMMockLibInfo.h"
#include "Logger.h"
#include "ModuleManager.pb.h"

namespace {
const std::string &GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(COMMockLibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(COMMockLibInfo.VERSION_MAJOR);
    version->set_minor(COMMockLibInfo.VERSION_MINOR);
    version->set_patch(COMMockLibInfo.VERSION_PATCH);
    version->set_version(COMMockLibInfo.VERSION);
    return manifest.SerializeAsString();
  }();
  return kSerialized;
}

std::string makePortLabel(const std::string &name, const std::string &devPath, size_t index) {
  if (!name.empty()) {
    return name;
  }
  if (!devPath.empty()) {
    return devPath;
  }
  return std::string("port-") + std::to_string(index);
}

void closeFd(int *fd) {
  if (fd != nullptr && *fd >= 0) {
    ::close(*fd);
    *fd = -1;
  }
}
}  // namespace

namespace COMMock {
COMMock::COMMock() :
  ModuleInterface(),
  comMockService_(std::make_shared<COMMockGrpcServiceImpl>()) {
  initLibInfo(COMMockLibInfo);
}

COMMock::~COMMock() {
  std::lock_guard<std::mutex> lock(ports_mutex_);
  cleanupPorts(&ports_);
}

void COMMock::start(std::stop_token stopToken) {
  ModuleManager::LogModuleScope scope(metaData_.name);
  LOG_INFO("COMMock 模块启动");
  comMockService_->getCOMMock(this);
  grpcServerBuilder(comMockService_);
  LOG_INFO("COMMock 等待配置下发（由 ConfigPusher 触发）");
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  {
    std::lock_guard<std::mutex> lock(ports_mutex_);
    cleanupPorts(&ports_);
  }
  LOG_INFO("COMMock 模块停止");
}

grpc::Status COMMock::ApplyConfig(const COMMockProto::COMMockConfig &config) {
  std::vector<PortConfig> configs;
  configs.reserve(static_cast<size_t>(config.ports_size()));

  size_t index = 0;
  for (const auto &port : config.ports()) {
    PortConfig cfg;
    cfg.name = port.name();
    cfg.dev_path = port.dev_path();
    if (cfg.dev_path.empty()) {
      LOG_ERROR("COMMock 配置缺少 dev_path: index={}, name={}", index, cfg.name);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "dev_path is required");
    }
    configs.push_back(std::move(cfg));
    ++index;
  }

  LOG_INFO("COMMock 收到配置下发: ports={}", configs.size());

  std::lock_guard<std::mutex> lock(ports_mutex_);
  cleanupPorts(&ports_);

  std::vector<PortHandle> new_ports;
  new_ports.reserve(configs.size());
  for (size_t i = 0; i < configs.size(); ++i) {
    PortHandle handle;
    if (!createPort(configs[i], i, &handle)) {
      LOG_ERROR("COMMock 创建虚拟串口失败，将终止本次配置: index={}, dev_path={}", i, configs[i].dev_path);
      cleanupPorts(&new_ports);
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "failed to create port");
    }
    new_ports.push_back(std::move(handle));
  }

  ports_ = std::move(new_ports);
  LOG_INFO("COMMock 配置下发生效: ports={}", ports_.size());
  return grpc::Status::OK;
}

bool COMMock::createPort(const PortConfig &config, size_t index, PortHandle *handle) {
  if (handle == nullptr) {
    return false;
  }

  const std::string label = makePortLabel(config.name, config.dev_path, index);
  if (config.dev_path.empty()) {
    LOG_ERROR("COMMock 端口配置 dev_path 为空: name={}", label);
    return false;
  }

  const std::filesystem::path dev_path(config.dev_path);
  if (!dev_path.is_absolute()) {
    LOG_INFO("COMMock dev_path 使用相对路径: name={}, dev_path={}", label, dev_path.string());
  }

  LOG_INFO("COMMock 创建虚拟串口: name={}, dev_path={}", label, dev_path.string());

  int master_fd = ::posix_openpt(O_RDWR | O_NOCTTY);
  if (master_fd < 0) {
    LOG_ERROR("COMMock 打开 PTY 主端失败: name={}, dev_path={}, errno={}, err={}",
              label, dev_path.string(), errno, std::strerror(errno));
    return false;
  }

  if (::grantpt(master_fd) != 0) {
    LOG_ERROR("COMMock grantpt 失败: name={}, dev_path={}, errno={}, err={}",
              label, dev_path.string(), errno, std::strerror(errno));
    closeFd(&master_fd);
    return false;
  }

  if (::unlockpt(master_fd) != 0) {
    LOG_ERROR("COMMock unlockpt 失败: name={}, dev_path={}, errno={}, err={}",
              label, dev_path.string(), errno, std::strerror(errno));
    closeFd(&master_fd);
    return false;
  }

  char *slave_name = ::ptsname(master_fd);
  if (slave_name == nullptr) {
    LOG_ERROR("COMMock 获取 PTY 从端失败: name={}, dev_path={}, errno={}, err={}",
              label, dev_path.string(), errno, std::strerror(errno));
    closeFd(&master_fd);
    return false;
  }

  int slave_fd = ::open(slave_name, O_RDWR | O_NOCTTY);
  if (slave_fd < 0) {
    LOG_ERROR("COMMock 打开 PTY 从端失败: name={}, dev_path={}, slave_path={}, errno={}, err={}",
              label, dev_path.string(), slave_name, errno, std::strerror(errno));
    closeFd(&master_fd);
    return false;
  }

  std::error_code ec;
  auto parent = dev_path.parent_path();
  if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
    if (ec) {
      LOG_ERROR("COMMock 检查 dev_path 目录失败: path={}, err={}", parent.string(), ec.message());
      closeFd(&slave_fd);
      closeFd(&master_fd);
      return false;
    }
    if (!std::filesystem::create_directories(parent, ec)) {
      if (ec) {
        LOG_ERROR("COMMock 创建 dev_path 目录失败: path={}, err={}", parent.string(), ec.message());
      } else {
        LOG_ERROR("COMMock 创建 dev_path 目录失败: path={}", parent.string());
      }
      closeFd(&slave_fd);
      closeFd(&master_fd);
      return false;
    }
  } else if (ec) {
    LOG_ERROR("COMMock 检查 dev_path 目录失败: path={}, err={}", parent.string(), ec.message());
    closeFd(&slave_fd);
    closeFd(&master_fd);
    return false;
  }

  if (std::filesystem::exists(dev_path, ec)) {
    if (ec) {
      LOG_ERROR("COMMock 检查 dev_path 失败: path={}, err={}", dev_path.string(), ec.message());
      closeFd(&slave_fd);
      closeFd(&master_fd);
      return false;
    }
    if (std::filesystem::is_symlink(dev_path, ec)) {
      std::filesystem::remove(dev_path, ec);
      if (ec) {
        LOG_ERROR("COMMock 移除旧 dev_path 失败: path={}, err={}", dev_path.string(), ec.message());
        closeFd(&slave_fd);
        closeFd(&master_fd);
        return false;
      }
      LOG_INFO("COMMock 已移除旧 dev_path 软链: path={}", dev_path.string());
    } else {
      LOG_ERROR("COMMock dev_path 已存在且非软链: name={}, path={}", label, dev_path.string());
      closeFd(&slave_fd);
      closeFd(&master_fd);
      return false;
    }
  } else if (ec) {
    LOG_ERROR("COMMock 检查 dev_path 失败: path={}, err={}", dev_path.string(), ec.message());
    closeFd(&slave_fd);
    closeFd(&master_fd);
    return false;
  }

  if (::symlink(slave_name, dev_path.c_str()) != 0) {
    LOG_ERROR("COMMock 创建 dev_path 软链失败: path={}, target={}, errno={}, err={}",
              dev_path.string(), slave_name, errno, std::strerror(errno));
    closeFd(&slave_fd);
    closeFd(&master_fd);
    return false;
  }

  LOG_INFO("COMMock 虚拟串口创建成功: name={}, dev_path={}, slave_path={}",
           label, dev_path.string(), slave_name);

  handle->config = config;
  handle->master_fd = master_fd;
  handle->slave_fd = slave_fd;
  handle->slave_path = slave_name;
  handle->symlink_created = true;
  return true;
}

void COMMock::cleanupPorts(std::vector<PortHandle> *ports) {
  if (ports == nullptr || ports->empty()) {
    return;
  }

  LOG_INFO("COMMock 开始清理虚拟串口: count={}", ports->size());
  for (size_t i = 0; i < ports->size(); ++i) {
    auto &port = (*ports)[i];
    const std::string label = makePortLabel(port.config.name, port.config.dev_path, i);
    if (port.symlink_created) {
      std::error_code ec;
      if (std::filesystem::is_symlink(port.config.dev_path, ec)) {
        if (!std::filesystem::remove(port.config.dev_path, ec)) {
          if (ec) {
            LOG_ERROR("COMMock 移除 dev_path 失败: name={}, path={}, err={}",
                      label, port.config.dev_path, ec.message());
          } else {
            LOG_INFO("COMMock dev_path 已不存在: name={}, path={}", label, port.config.dev_path);
          }
        } else {
          LOG_INFO("COMMock 已移除 dev_path 软链: name={}, path={}", label, port.config.dev_path);
        }
      } else if (ec) {
        LOG_ERROR("COMMock 检查 dev_path 失败: name={}, path={}, err={}",
                  label, port.config.dev_path, ec.message());
      } else {
        LOG_INFO("COMMock dev_path 非软链，跳过删除: name={}, path={}", label, port.config.dev_path);
      }
    }

    closeFd(&port.slave_fd);
    closeFd(&port.master_fd);
  }
  ports->clear();
  LOG_INFO("COMMock 虚拟串口清理完成");
}
}  // namespace COMMock

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new COMMock::COMMock();
}

extern "C" BOOST_SYMBOL_EXPORT bool GetModuleManifestPb(const uint8_t **data, size_t *size) {
  if (data == nullptr || size == nullptr) {
    return false;
  }
  const auto &serialized = GetSerializedManifest();
  *data = reinterpret_cast<const uint8_t *>(serialized.data());
  *size = serialized.size();
  return true;
}
