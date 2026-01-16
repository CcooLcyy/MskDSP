#include "ConfigPusher.h"

#include <google/protobuf/util/json_util.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <boost/dll.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "ConfigPusher.pb.h"
#include "ConfigPusherGrpcService.h"
#include "ConfigPusherLibInfo.h"
#include "IEC104.grpc.pb.h"
#include "Logger.h"
#include "ModbusRTU.grpc.pb.h"
#include "ModuleManager.grpc.pb.h"
#include "ModuleManager.pb.h"

namespace {
const std::string &GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(ConfigPusherLibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(ConfigPusherLibInfo.VERSION_MAJOR);
    version->set_minor(ConfigPusherLibInfo.VERSION_MINOR);
    version->set_patch(ConfigPusherLibInfo.VERSION_PATCH);
    version->set_version(ConfigPusherLibInfo.VERSION);
    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace ConfigPusher {
namespace {
constexpr const char *kIec104ConfigPath = "./conf/configPusher/iec104.jsonc";
constexpr const char *kModbusRtuConfigPath = "./conf/configPusher/modbus_rtu.jsonc";
constexpr const char *kModuleManagerAddress = "127.0.0.1:7000";
constexpr const char *kDataCenterModuleName = "DataCenter";
constexpr const char *kIec104ModuleName = "IEC104";
constexpr const char *kModbusRtuModuleName = "ModbusRTU";
constexpr auto kModulePollInterval = std::chrono::milliseconds(200);
constexpr auto kModuleStartTimeout = std::chrono::seconds(5);

std::string stripJsonComments(std::string_view input) {
  std::string out;
  out.reserve(input.size());

  bool inString = false;
  bool escape = false;
  bool inLineComment = false;
  bool inBlockComment = false;

  for (size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];

    if (inLineComment) {
      if (c == '\n') {
        inLineComment = false;
        out.push_back(c);
      }
      continue;
    }

    if (inBlockComment) {
      if (c == '*' && i + 1 < input.size() && input[i + 1] == '/') {
        inBlockComment = false;
        ++i;
        continue;
      }
      if (c == '\n') {
        out.push_back(c);
      }
      continue;
    }

    if (inString) {
      out.push_back(c);
      if (escape) {
        escape = false;
        continue;
      }
      if (c == '\\') {
        escape = true;
        continue;
      }
      if (c == '"') {
        inString = false;
      }
      continue;
    }

    if (c == '"') {
      inString = true;
      out.push_back(c);
      continue;
    }

    if (c == '/' && i + 1 < input.size()) {
      const char next = input[i + 1];
      if (next == '/') {
        inLineComment = true;
        ++i;
        continue;
      }
      if (next == '*') {
        inBlockComment = true;
        ++i;
        continue;
      }
    }

    out.push_back(c);
  }

  return out;
}

bool readFile(const std::filesystem::path &path, std::string *out) {
  if (out == nullptr) {
    return false;
  }
  std::ifstream ifs(path, std::ios::in | std::ios::binary);
  if (!ifs.is_open()) {
    return false;
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  *out = oss.str();
  return true;
}

std::optional<ConfigPusherProto::Config> loadConfigFile(const std::filesystem::path &path) {
  if (!std::filesystem::exists(path)) {
    LOG_INFO("未找到 ConfigPusher 配置文件: {}", path.string());
    return std::nullopt;
  }

  LOG_INFO("开始读取 ConfigPusher 配置文件: {}", path.string());
  std::string raw;
  if (!readFile(path, &raw)) {
    LOG_ERROR("读取 ConfigPusher 配置文件失败: {}", path.string());
    return std::nullopt;
  }

  auto json = stripJsonComments(raw);
  ConfigPusherProto::Config config;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = false;
  auto parseStatus = google::protobuf::util::JsonStringToMessage(json, &config, options);
  if (!parseStatus.ok()) {
    LOG_ERROR("解析 ConfigPusher 配置失败: {}", parseStatus.ToString());
    return std::nullopt;
  }
  return config;
}

std::optional<ModuleManagerProto::ModuleInfo> findModuleInfo(
    const ModuleManagerProto::ModuleInfos &infos, std::string_view moduleName) {
  for (const auto &info : infos.module_info()) {
    if (info.module_name() == moduleName) {
      return info;
    }
  }
  return std::nullopt;
}

std::optional<ModuleManagerProto::ModuleRunningInfo> findRunningInfo(
    const ModuleManagerProto::ModuleRunningInfos &infos, std::string_view moduleName) {
  for (const auto &info : infos.module_running_info()) {
    if (info.module_name() == moduleName) {
      return info;
    }
  }
  return std::nullopt;
}

std::optional<ModuleManagerProto::ModuleRunningInfo> waitForModule(
    ModuleManagerProto::ModuleManage::StubInterface *stub,
    std::string_view moduleName,
    std::chrono::milliseconds timeout) {
  if (stub == nullptr) {
    return std::nullopt;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    ModuleManagerProto::ModuleRunningInfos running;
    grpc::ClientContext ctx;
    ModuleManagerProto::Empty req;
    auto status = stub->GetRunningModuleInfo(&ctx, req, &running);
    if (!status.ok()) {
      return std::nullopt;
    }
    auto found = findRunningInfo(running, moduleName);
    if (found) {
      return found;
    }
    std::this_thread::sleep_for(kModulePollInterval);
  }
  return std::nullopt;
}

bool applyIec104Config(const ConfigPusherProto::Iec104Config &config, IEC104Proto::IEC104Service::StubInterface *stub) {
  if (stub == nullptr) {
    LOG_ERROR("IEC104 gRPC stub 为空");
    return false;
  }

  bool ok = true;
  for (const auto &task : config.links()) {
    if (!task.has_link() || !task.link().has_config()) {
      LOG_ERROR("IEC104 配置任务缺少 link/config");
      ok = false;
      continue;
    }

    const auto &linkConfig = task.link().config();
    if (linkConfig.conn_name().empty()) {
      LOG_ERROR("IEC104 配置任务缺少 config.conn_name");
      ok = false;
      continue;
    }

    LOG_INFO("开始下发 IEC104 连接配置: conn_name={}", linkConfig.conn_name());
    IEC104Proto::UpsertLinkRequest linkReq = task.link();
    IEC104Proto::LinkInfo linkInfo;
    grpc::ClientContext linkCtx;
    auto status = stub->UpsertLink(&linkCtx, linkReq, &linkInfo);
    if (!status.ok()) {
      LOG_ERROR("IEC104 连接配置失败: conn_name={}, 原因={}", linkConfig.conn_name(), status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("IEC104 连接配置成功: conn_name={}, conn_id={}", linkConfig.conn_name(), linkInfo.conn_id());

    if (task.has_point_table() && task.point_table().points_size() > 0) {
      IEC104Proto::UpsertPointTableRequest ptReq = task.point_table();
      if (ptReq.conn_name().empty()) {
        ptReq.set_conn_name(linkConfig.conn_name());
      }
      LOG_INFO("开始下发 IEC104 点表: conn_name={}, 点数={}, replace={}", ptReq.conn_name(), ptReq.points_size(), ptReq.replace());
      grpc::ClientContext ptCtx;
      IEC104Proto::Empty ptResp;
      status = stub->UpsertPointTable(&ptCtx, ptReq, &ptResp);
      if (!status.ok()) {
        LOG_ERROR("IEC104 点表下发失败: conn_name={}, 原因={}", ptReq.conn_name(), status.error_message());
        ok = false;
        continue;
      }
      LOG_INFO("IEC104 点表下发成功: conn_name={}, 点数={}", ptReq.conn_name(), ptReq.points_size());
    }

    if (task.start()) {
      IEC104Proto::StartLinkRequest startReq;
      startReq.set_conn_name(linkConfig.conn_name());
      grpc::ClientContext startCtx;
      IEC104Proto::Empty startResp;
      status = stub->StartLink(&startCtx, startReq, &startResp);
      if (!status.ok()) {
        LOG_ERROR("IEC104 启动连接失败: conn_name={}, 原因={}", linkConfig.conn_name(), status.error_message());
        ok = false;
        continue;
      }
      LOG_INFO("IEC104 连接启动成功: conn_name={}", linkConfig.conn_name());
    }
  }

  return ok;
}

bool applyModbusRtuConfig(const ConfigPusherProto::ModbusRtuConfig &config, ModbusRTUProto::ModbusRTUService::StubInterface *stub) {
  if (stub == nullptr) {
    LOG_ERROR("ModbusRTU gRPC stub 为空");
    return false;
  }

  bool ok = true;
  for (const auto &task : config.links()) {
    if (!task.has_link() || !task.link().has_config()) {
      LOG_ERROR("ModbusRTU 配置任务缺少 link/config");
      ok = false;
      continue;
    }

    const auto &linkConfig = task.link().config();
    if (linkConfig.conn_name().empty()) {
      LOG_ERROR("ModbusRTU 配置任务缺少 config.conn_name");
      ok = false;
      continue;
    }

    LOG_INFO("开始下发 ModbusRTU 连接配置: conn_name={}", linkConfig.conn_name());
    ModbusRTUProto::UpsertLinkRequest linkReq = task.link();
    ModbusRTUProto::LinkInfo linkInfo;
    grpc::ClientContext linkCtx;
    auto status = stub->UpsertLink(&linkCtx, linkReq, &linkInfo);
    if (!status.ok()) {
      LOG_ERROR("ModbusRTU 连接配置失败: conn_name={}, 原因={}", linkConfig.conn_name(), status.error_message());
      ok = false;
      continue;
    }
    LOG_INFO("ModbusRTU 连接配置成功: conn_name={}, conn_id={}", linkConfig.conn_name(), linkInfo.conn_id());

    if (task.has_point_table() && task.point_table().points_size() > 0) {
      ModbusRTUProto::UpsertPointTableRequest ptReq = task.point_table();
      if (ptReq.conn_name().empty()) {
        ptReq.set_conn_name(linkConfig.conn_name());
      }
      LOG_INFO("开始下发 ModbusRTU 点表: conn_name={}, 点数={}, replace={}", ptReq.conn_name(), ptReq.points_size(), ptReq.replace());
      grpc::ClientContext ptCtx;
      ModbusRTUProto::Empty ptResp;
      status = stub->UpsertPointTable(&ptCtx, ptReq, &ptResp);
      if (!status.ok()) {
        LOG_ERROR("ModbusRTU 点表下发失败: conn_name={}, 原因={}", ptReq.conn_name(), status.error_message());
        ok = false;
        continue;
      }
      LOG_INFO("ModbusRTU 点表下发成功: conn_name={}, 点数={}", ptReq.conn_name(), ptReq.points_size());
    }

    if (task.start()) {
      ModbusRTUProto::StartLinkRequest startReq;
      startReq.set_conn_name(linkConfig.conn_name());
      grpc::ClientContext startCtx;
      ModbusRTUProto::Empty startResp;
      status = stub->StartLink(&startCtx, startReq, &startResp);
      if (!status.ok()) {
        LOG_ERROR("ModbusRTU 启动连接失败: conn_name={}, 原因={}", linkConfig.conn_name(), status.error_message());
        ok = false;
        continue;
      }
      LOG_INFO("ModbusRTU 连接启动成功: conn_name={}", linkConfig.conn_name());
    }
  }

  return ok;
}
}  // namespace

ConfigPusher::ConfigPusher() :
  ModuleInterface(),
  configPusherService_(std::make_shared<ConfigPusherGrpcServiceImpl>()) {
  initLibInfo(ConfigPusherLibInfo);
}

ConfigPusher::~ConfigPusher() {}

void ConfigPusher::start(std::stop_token stopToken) {
  LOG_INFO("ConfigPusher 模块启动");
  configPusherService_->getConfigPusher(this);
  grpcServerBuilder(configPusherService_);
  applyConfig();

  std::mutex mu;
  std::condition_variable_any cv;
  std::stop_callback cb(stopToken, [&cv]() { cv.notify_all(); });
  std::unique_lock lock(mu);
  cv.wait(lock, [&stopToken]() { return stopToken.stop_requested(); });
  LOG_INFO("ConfigPusher 模块停止");
}

void ConfigPusher::applyConfig() {
  auto iec104Config = loadConfigFile(kIec104ConfigPath);
  auto modbusConfig = loadConfigFile(kModbusRtuConfigPath);

  const bool hasIec104 = iec104Config && iec104Config->has_iec104() && !iec104Config->iec104().links().empty();
  const bool hasModbus = modbusConfig && modbusConfig->has_modbus_rtu() && !modbusConfig->modbus_rtu().links().empty();
  if (!hasIec104 && !hasModbus) {
    LOG_INFO("配置中未包含 IEC104/ModbusRTU 链路");
    return;
  }
  LOG_INFO("ConfigPusher 配置解析完成，开始准备下发配置");

  auto channel = grpc::CreateChannel(kModuleManagerAddress, grpc::InsecureChannelCredentials());
  auto moduleStub = ModuleManagerProto::ModuleManage::NewStub(channel);

  ModuleManagerProto::ModuleInfos moduleInfos;
  grpc::ClientContext infoCtx;
  ModuleManagerProto::Empty infoReq;
  auto status = moduleStub->GetModuleInfo(&infoCtx, infoReq, &moduleInfos);
  if (!status.ok()) {
    LOG_ERROR("获取模块列表失败: {}", status.error_message());
    return;
  }

  auto dataCenterInfo = findModuleInfo(moduleInfos, kDataCenterModuleName);
  if (!dataCenterInfo) {
    LOG_ERROR("未找到模块: {}", kDataCenterModuleName);
    return;
  }
  std::optional<ModuleManagerProto::ModuleInfo> iec104Info;
  if (hasIec104) {
    iec104Info = findModuleInfo(moduleInfos, kIec104ModuleName);
    if (!iec104Info) {
      LOG_ERROR("未找到模块: {}", kIec104ModuleName);
      return;
    }
  }
  std::optional<ModuleManagerProto::ModuleInfo> modbusInfo;
  if (hasModbus) {
    modbusInfo = findModuleInfo(moduleInfos, kModbusRtuModuleName);
    if (!modbusInfo) {
      LOG_ERROR("未找到模块: {}", kModbusRtuModuleName);
      return;
    }
  }

  ModuleManagerProto::ModuleRunningInfos running;
  grpc::ClientContext runningCtx;
  ModuleManagerProto::Empty runningReq;
  status = moduleStub->GetRunningModuleInfo(&runningCtx, runningReq, &running);
  if (!status.ok()) {
    LOG_ERROR("获取运行中模块信息失败: {}", status.error_message());
    return;
  }

  auto runningDataCenter = findRunningInfo(running, kDataCenterModuleName);
  if ((hasIec104 || hasModbus) && !runningDataCenter) {
    LOG_INFO("DataCenter 未运行，开始启动");
    grpc::ClientContext startCtx;
    ModuleManagerProto::Empty startResp;
    status = moduleStub->StartModule(&startCtx, *dataCenterInfo, &startResp);
    if (!status.ok()) {
      LOG_ERROR("启动模块 {} 失败: {}", kDataCenterModuleName, status.error_message());
      return;
    }
    runningDataCenter = waitForModule(moduleStub.get(), kDataCenterModuleName, kModuleStartTimeout);
    if (!runningDataCenter) {
      LOG_ERROR("等待 DataCenter 启动超时");
      return;
    }
    LOG_INFO("DataCenter 已启动");
  } else if (hasIec104 || hasModbus) {
    LOG_INFO("DataCenter 已在运行");
  }

  std::optional<ModuleManagerProto::ModuleRunningInfo> runningIec104;
  if (hasIec104) {
    runningIec104 = findRunningInfo(running, kIec104ModuleName);
    if (!runningIec104) {
      LOG_INFO("IEC104 未运行，开始启动");
      grpc::ClientContext startCtx;
      ModuleManagerProto::Empty startResp;
      status = moduleStub->StartModule(&startCtx, *iec104Info, &startResp);
      if (!status.ok()) {
        LOG_ERROR("启动模块 {} 失败: {}", kIec104ModuleName, status.error_message());
        return;
      }
      runningIec104 = waitForModule(moduleStub.get(), kIec104ModuleName, kModuleStartTimeout);
      if (!runningIec104) {
        LOG_ERROR("等待 IEC104 启动超时");
        return;
      }
      LOG_INFO("IEC104 已启动");
    } else {
      LOG_INFO("IEC104 已在运行");
    }
  }

  std::optional<ModuleManagerProto::ModuleRunningInfo> runningModbus;
  if (hasModbus) {
    runningModbus = findRunningInfo(running, kModbusRtuModuleName);
    if (!runningModbus) {
      LOG_INFO("ModbusRTU 未运行，开始启动");
      grpc::ClientContext startCtx;
      ModuleManagerProto::Empty startResp;
      status = moduleStub->StartModule(&startCtx, *modbusInfo, &startResp);
      if (!status.ok()) {
        LOG_ERROR("启动模块 {} 失败: {}", kModbusRtuModuleName, status.error_message());
        return;
      }
      runningModbus = waitForModule(moduleStub.get(), kModbusRtuModuleName, kModuleStartTimeout);
      if (!runningModbus) {
        LOG_ERROR("等待 ModbusRTU 启动超时");
        return;
      }
      LOG_INFO("ModbusRTU 已启动");
    } else {
      LOG_INFO("ModbusRTU 已在运行");
    }
  }

  if (hasIec104 && runningIec104) {
    auto iecChannel = grpc::CreateChannel(runningIec104->inner_grpc_server(), grpc::InsecureChannelCredentials());
    auto iecStub = IEC104Proto::IEC104Service::NewStub(iecChannel);
    if (!applyIec104Config(iec104Config->iec104(), iecStub.get())) {
      LOG_ERROR("IEC104 配置下发存在错误");
    } else {
      LOG_INFO("IEC104 配置下发完成");
    }
  }

  if (hasModbus && runningModbus) {
    auto modbusChannel = grpc::CreateChannel(runningModbus->inner_grpc_server(), grpc::InsecureChannelCredentials());
    auto modbusStub = ModbusRTUProto::ModbusRTUService::NewStub(modbusChannel);
    if (!applyModbusRtuConfig(modbusConfig->modbus_rtu(), modbusStub.get())) {
      LOG_ERROR("ModbusRTU 配置下发存在错误");
    } else {
      LOG_INFO("ModbusRTU 配置下发完成");
    }
  }
}
}  // namespace ConfigPusher

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface *create() {
  return new ConfigPusher::ConfigPusher();
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
