#include "COMMock.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#include <array>
#include <boost/dll.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "COMMock.pb.h"
#include "COMMockGrpcService.h"
#include "COMMockLibInfo.h"
#include "Logger.h"
#include "ModuleManager.pb.h"

extern char **environ;

namespace {
constexpr std::chrono::milliseconds kSocatReadyInterval(50);
constexpr std::chrono::milliseconds kSocatReadyTimeout(1000);

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

bool waitForSymlink(const std::filesystem::path &path, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::error_code ec;
    if (std::filesystem::is_symlink(path, ec)) {
      return true;
    }
    std::this_thread::sleep_for(kSocatReadyInterval);
  }
  return false;
}
}  // namespace

namespace COMMock {
COMMock::COMMock() :
  ModuleInterface(),
  comMockService_(std::make_shared<COMMockGrpcServiceImpl>()) {
  initLibInfo(COMMockLibInfo);
}

COMMock::~COMMock() {
  std::lock_guard<std::mutex> lock(pairs_mutex_);
  cleanupPairs(&pairs_);
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
    std::lock_guard<std::mutex> lock(pairs_mutex_);
    cleanupPairs(&pairs_);
  }
  LOG_INFO("COMMock 模块停止");
}

grpc::Status COMMock::ApplyConfig(const COMMockProto::COMMockConfig &config) {
  std::unordered_map<std::string, PortConfig> ports_by_name;
  ports_by_name.reserve(static_cast<size_t>(config.ports_size()));
  std::unordered_set<std::string> dev_paths;
  dev_paths.reserve(static_cast<size_t>(config.ports_size()));

  size_t index = 0;
  for (const auto &port : config.ports()) {
    PortConfig cfg;
    cfg.name = port.name();
    cfg.dev_path = port.dev_path();
    cfg.peer_name = port.peer_name();
    cfg.index = index;

    if (cfg.name.empty()) {
      LOG_ERROR("COMMock 配置缺少 name: index={}, dev_path={}", index, cfg.dev_path);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "name 不能为空");
    }
    if (cfg.dev_path.empty()) {
      LOG_ERROR("COMMock 配置缺少 dev_path: index={}, name={}", index, cfg.name);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "dev_path 不能为空");
    }
    if (cfg.peer_name.empty()) {
      LOG_ERROR("COMMock 配置缺少 peer_name: index={}, name={}", index, cfg.name);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "peer_name 不能为空");
    }
    if (cfg.peer_name == cfg.name) {
      LOG_ERROR("COMMock peer_name 与 name 相同: name={}", cfg.name);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "peer_name 不能等于 name");
    }
    if (!dev_paths.emplace(cfg.dev_path).second) {
      LOG_ERROR("COMMock dev_path 重复: name={}, dev_path={}", cfg.name, cfg.dev_path);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "dev_path 重复");
    }
    auto [it, inserted] = ports_by_name.emplace(cfg.name, cfg);
    if (!inserted) {
      LOG_ERROR("COMMock 端口 name 重复: name={}", cfg.name);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "name 重复");
    }
    ++index;
  }

  LOG_INFO("COMMock 收到配置下发: ports={}", ports_by_name.size());

  std::vector<PortPair> pair_configs;
  pair_configs.reserve(ports_by_name.size() / 2);
  std::unordered_set<std::string> visited;
  visited.reserve(ports_by_name.size());

  for (const auto &entry : ports_by_name) {
    const auto &cfg = entry.second;
    if (visited.count(cfg.name) > 0) {
      continue;
    }
    auto it_peer = ports_by_name.find(cfg.peer_name);
    if (it_peer == ports_by_name.end()) {
      LOG_ERROR("COMMock 端口 peer_name 不存在: name={}, peer_name={}", cfg.name, cfg.peer_name);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "peer_name 不存在");
    }
    const auto &peer = it_peer->second;
    if (peer.peer_name != cfg.name) {
      LOG_ERROR("COMMock 端口 peer_name 未互指: name={}, peer_name={}, peer_peer={}", cfg.name, cfg.peer_name, peer.peer_name);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "peer_name 未互指");
    }
    visited.insert(cfg.name);
    visited.insert(peer.name);

    PortPair pair;
    pair.left = cfg;
    pair.right = peer;
    pair_configs.push_back(std::move(pair));
  }

  if (!ports_by_name.empty() && visited.size() != ports_by_name.size()) {
    LOG_ERROR("COMMock 端口未能完全配对: ports={}, paired={}", ports_by_name.size(), visited.size());
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "端口配对不完整");
  }

  LOG_INFO("COMMock 端口配对完成: pairs={}", pair_configs.size());

  std::lock_guard<std::mutex> lock(pairs_mutex_);
  cleanupPairs(&pairs_);

  if (pair_configs.empty()) {
    LOG_INFO("COMMock 配置为空，已清理虚拟串口");
    return grpc::Status::OK;
  }

  std::vector<PortPair> new_pairs;
  new_pairs.reserve(pair_configs.size());
  for (auto &pair : pair_configs) {
    if (!prepareDevPath(pair.left) || !prepareDevPath(pair.right)) {
      LOG_ERROR("COMMock 准备 dev_path 失败，将终止本次配置: left={}, right={}", makePortLabel(pair.left.name, pair.left.dev_path, pair.left.index), makePortLabel(pair.right.name, pair.right.dev_path, pair.right.index));
      cleanupPairs(&new_pairs);
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "准备 dev_path 失败");
    }
    if (!startSocatPair(pair.left, pair.right, &pair)) {
      LOG_ERROR("COMMock 启动 socat 失败，将终止本次配置: left={}, right={}", makePortLabel(pair.left.name, pair.left.dev_path, pair.left.index), makePortLabel(pair.right.name, pair.right.dev_path, pair.right.index));
      cleanupPairs(&new_pairs);
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "启动 socat 失败");
    }
    new_pairs.push_back(std::move(pair));
  }

  pairs_ = std::move(new_pairs);
  LOG_INFO("COMMock 配置下发生效: pairs={}", pairs_.size());
  return grpc::Status::OK;
}

bool COMMock::prepareDevPath(const PortConfig &config) {
  const std::string label = makePortLabel(config.name, config.dev_path, config.index);
  if (config.dev_path.empty()) {
    LOG_ERROR("COMMock 端口配置 dev_path 为空: name={}", label);
    return false;
  }

  const std::filesystem::path dev_path(config.dev_path);
  if (!dev_path.is_absolute()) {
    LOG_INFO("COMMock dev_path 使用相对路径: name={}, dev_path={}", label, dev_path.string());
  }

  std::error_code ec;
  auto parent = dev_path.parent_path();
  if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
    if (ec) {
      LOG_ERROR("COMMock 检查 dev_path 目录失败: path={}, 错误={}", parent.string(), ec.message());
      return false;
    }
    if (!std::filesystem::create_directories(parent, ec)) {
      if (ec) {
        LOG_ERROR("COMMock 创建 dev_path 目录失败: path={}, 错误={}", parent.string(), ec.message());
      } else {
        LOG_ERROR("COMMock 创建 dev_path 目录失败: path={}", parent.string());
      }
      return false;
    }
  } else if (ec) {
    LOG_ERROR("COMMock 检查 dev_path 目录失败: path={}, 错误={}", parent.string(), ec.message());
    return false;
  }

  if (std::filesystem::exists(dev_path, ec)) {
    if (ec) {
      LOG_ERROR("COMMock 检查 dev_path 失败: path={}, 错误={}", dev_path.string(), ec.message());
      return false;
    }
    if (std::filesystem::is_symlink(dev_path, ec)) {
      std::filesystem::remove(dev_path, ec);
      if (ec) {
        LOG_ERROR("COMMock 移除旧 dev_path 失败: path={}, 错误={}", dev_path.string(), ec.message());
        return false;
      }
      LOG_INFO("COMMock 已移除旧 dev_path 软链: path={}", dev_path.string());
    } else {
      LOG_ERROR("COMMock dev_path 已存在且非软链: name={}, path={}", label, dev_path.string());
      return false;
    }
  } else if (ec) {
    LOG_ERROR("COMMock 检查 dev_path 失败: path={}, 错误={}", dev_path.string(), ec.message());
    return false;
  }

  return true;
}

bool COMMock::startSocatPair(const PortConfig &left, const PortConfig &right, PortPair *pair) {
  if (pair == nullptr) {
    LOG_ERROR("COMMock 启动 socat 失败: pair 为空");
    return false;
  }
  const std::string left_label = makePortLabel(left.name, left.dev_path, left.index);
  const std::string right_label = makePortLabel(right.name, right.dev_path, right.index);
  LOG_INFO("COMMock 启动 socat 互通: left={}, right={}", left_label, right_label);

  const std::string left_arg = "pty,raw,echo=0,link=" + left.dev_path;
  const std::string right_arg = "pty,raw,echo=0,link=" + right.dev_path;
  std::array<std::string, 3> args = {"socat", left_arg, right_arg};
  std::array<char *, 4> argv = {const_cast<char *>(args[0].c_str()), const_cast<char *>(args[1].c_str()), const_cast<char *>(args[2].c_str()), nullptr};

  pid_t pid = -1;
  const int rc = ::posix_spawnp(&pid, "socat", nullptr, nullptr, argv.data(), environ);
  if (rc != 0) {
    if (rc == ENOENT) {
      LOG_ERROR("COMMock 启动 socat 失败: left={}, right={}, 错误=PATH 中未找到 socat", left_label, right_label);
    } else {
      LOG_ERROR("COMMock 启动 socat 失败: left={}, right={}, 错误={}", left_label, right_label, std::strerror(rc));
    }
    return false;
  }

  if (!waitForSymlink(left.dev_path, kSocatReadyTimeout) ||
      !waitForSymlink(right.dev_path, kSocatReadyTimeout)) {
    LOG_ERROR("COMMock 等待 socat 创建 dev_path 超时: left={}, right={}, pid={}", left_label, right_label, pid);
    PortPair temp;
    temp.left = left;
    temp.right = right;
    temp.pid = pid;
    stopSocat(temp);
    cleanupDevPath(left);
    cleanupDevPath(right);
    return false;
  }

  pair->left = left;
  pair->right = right;
  pair->pid = pid;
  LOG_INFO("COMMock socat 互通已启动: left={}, right={}, pid={}", left_label, right_label, pid);
  return true;
}

void COMMock::cleanupPairs(std::vector<PortPair> *pairs) {
  if (pairs == nullptr || pairs->empty()) {
    return;
  }

  LOG_INFO("COMMock 开始清理虚拟串口互通: 数量={}", pairs->size());
  for (const auto &pair : *pairs) {
    stopSocat(pair);
    cleanupDevPath(pair.left);
    cleanupDevPath(pair.right);
  }
  pairs->clear();
  LOG_INFO("COMMock 虚拟串口清理完成");
}

void COMMock::cleanupDevPath(const PortConfig &config) {
  if (config.dev_path.empty()) {
    return;
  }
  const std::string label = makePortLabel(config.name, config.dev_path, config.index);
  std::error_code ec;
  if (std::filesystem::is_symlink(config.dev_path, ec)) {
    if (!std::filesystem::remove(config.dev_path, ec)) {
      if (ec) {
        LOG_ERROR("COMMock 移除 dev_path 失败: name={}, path={}, 错误={}", label, config.dev_path, ec.message());
      } else {
        LOG_INFO("COMMock dev_path 已不存在: name={}, path={}", label, config.dev_path);
      }
    } else {
      LOG_INFO("COMMock 已移除 dev_path 软链: name={}, path={}", label, config.dev_path);
    }
  } else if (ec) {
    LOG_ERROR("COMMock 检查 dev_path 失败: name={}, path={}, 错误={}", label, config.dev_path, ec.message());
  } else {
    LOG_INFO("COMMock dev_path 非软链，跳过删除: name={}, path={}", label, config.dev_path);
  }
}

void COMMock::stopSocat(const PortPair &pair) {
  if (pair.pid <= 0) {
    return;
  }
  const std::string left_label = makePortLabel(pair.left.name, pair.left.dev_path, pair.left.index);
  const std::string right_label = makePortLabel(pair.right.name, pair.right.dev_path, pair.right.index);
  LOG_INFO("COMMock 停止 socat 互通: left={}, right={}, pid={}", left_label, right_label, pair.pid);

  if (::kill(pair.pid, SIGTERM) != 0) {
    if (errno != ESRCH) {
      LOG_ERROR("COMMock 终止 socat 失败: pid={}, 错误={}", pair.pid, std::strerror(errno));
    }
    return;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (true) {
    int status = 0;
    const pid_t result = ::waitpid(pair.pid, &status, WNOHANG);
    if (result == pair.pid) {
      return;
    }
    if (result == 0) {
      if (std::chrono::steady_clock::now() >= deadline) {
        LOG_ERROR("COMMock 等待 socat 退出超时，将强制终止: pid={}", pair.pid);
        ::kill(pair.pid, SIGKILL);
        ::waitpid(pair.pid, nullptr, 0);
        return;
      }
      std::this_thread::sleep_for(kSocatReadyInterval);
      continue;
    }
    if (result < 0) {
      if (errno != ECHILD) {
        LOG_ERROR("COMMock 等待 socat 退出失败: pid={}, 错误={}", pair.pid, std::strerror(errno));
      }
      return;
    }
  }
}
}  // namespace COMMock

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface *create() {
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
