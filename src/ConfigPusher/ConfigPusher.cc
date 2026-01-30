#include "ConfigPusher.h"

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <boost/dll.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

#include "COMMock.grpc.pb.h"
#include "ConfigPusher.pb.h"
#include "ConfigPusherApplyComMock.h"
#include "ConfigPusherApplyDlt645.h"
#include "ConfigPusherApplyIec104.h"
#include "ConfigPusherApplyModbusRtu.h"
#include "ConfigPusherConfigLoader.h"
#include "ConfigPusherDataCenter.h"
#include "ConfigPusherGrpcService.h"
#include "ConfigPusherLibInfo.h"
#include "ConfigPusherModuleManager.h"
#include "DLT645.grpc.pb.h"
#include "DataCenter.grpc.pb.h"
#include "IEC104.grpc.pb.h"
#include "Logger.h"
#include "ModbusRTU.grpc.pb.h"
#include "ModuleManager.grpc.pb.h"

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
constexpr const char *kDlt645ConfigPath = "./conf/configPusher/DLT645.jsonc";
constexpr const char *kDataCenterConfigPath = "./conf/configPusher/DataCenter.jsonc";
constexpr const char *kComMockConfigPath = "./conf/configPusher/COMMock.jsonc";
constexpr const char *kModuleManagerAddress = "127.0.0.1:7000";
constexpr const char *kDataCenterModuleName = "DataCenter";
constexpr const char *kIec104ModuleName = "IEC104";
constexpr const char *kModbusRtuModuleName = "ModbusRTU";
constexpr const char *kDlt645ModuleName = "DLT645";
constexpr const char *kMqttManagerModuleName = "MQTTManager";
constexpr const char *kComMockModuleName = "COMMock";
constexpr auto kModuleStartTimeout = std::chrono::seconds(5);

std::optional<std::filesystem::path> ResolveConfigPusherDir() {
  try {
    auto libPath = boost::dll::this_line_location();
    std::filesystem::path modulePath(libPath.string());
    auto confDir = (modulePath.parent_path() / ".." / "conf" / "configPusher").lexically_normal();
    std::error_code ec;
    if (!std::filesystem::exists(confDir, ec)) {
      LOG_WARNING("ConfigPusher 未找到模块相对配置目录: {}，继续使用相对路径", confDir.string());
      return std::nullopt;
    }
    return confDir;
  } catch (const std::exception &ex) {
    LOG_WARNING("ConfigPusher 获取模块路径失败，继续使用相对路径: {}", ex.what());
    return std::nullopt;
  }
}

std::filesystem::path ResolveConfigPath(const std::optional<std::filesystem::path> &configDir,
                                        const char *fallbackPath) {
  if (!configDir.has_value()) {
    return std::filesystem::path(fallbackPath);
  }
  return *configDir / std::filesystem::path(fallbackPath).filename();
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
  auto configDir = ResolveConfigPusherDir();
  if (configDir.has_value()) {
    LOG_INFO("ConfigPusher 配置目录已解析: {}", configDir->string());
  }

  auto comMockConfig = LoadConfigFile(ResolveConfigPath(configDir, kComMockConfigPath));
  auto iec104Config = LoadConfigFile(ResolveConfigPath(configDir, kIec104ConfigPath));
  auto modbusConfig = LoadConfigFile(ResolveConfigPath(configDir, kModbusRtuConfigPath));
  auto dlt645Config = LoadConfigFile(ResolveConfigPath(configDir, kDlt645ConfigPath));
  auto dataCenterConfig = LoadDataCenterConfigFile(ResolveConfigPath(configDir, kDataCenterConfigPath));

  const bool hasComMock = comMockConfig && comMockConfig->has_com_mock() && comMockConfig->com_mock().ports_size() > 0;
  const bool hasIec104 = iec104Config && iec104Config->has_iec104() && !iec104Config->iec104().links().empty();
  const bool hasModbus = modbusConfig && modbusConfig->has_modbus_rtu() && !modbusConfig->modbus_rtu().links().empty();
  const bool hasDlt645 = dlt645Config && dlt645Config->has_dlt645() && !dlt645Config->dlt645().links().empty();
  const bool hasDataCenter = dataCenterConfig && (!dataCenterConfig->point_tables().empty() || (dataCenterConfig->has_routes() && dataCenterConfig->routes().routes_size() > 0));
  const bool needsDataCenter = hasIec104 || hasModbus || hasDlt645 || hasDataCenter;
  if (!hasComMock && !hasIec104 && !hasModbus && !hasDlt645 && !hasDataCenter) {
    LOG_INFO("配置中未包含 COMMock/IEC104/ModbusRTU/DLT645/DataCenter 配置");
    return;
  }
  LOG_INFO("ConfigPusher 配置解析完成，开始准备下发配置");

  auto channel = grpc::CreateChannel(kModuleManagerAddress, grpc::InsecureChannelCredentials());
  auto moduleStub = ModuleManagerProto::ModuleManage::NewStub(channel);

  ModuleManagerProto::ModuleInfos moduleInfos;
  if (!fetchModuleInfos(moduleStub.get(), &moduleInfos)) {
    LOG_ERROR("获取模块信息失败，终止下发");
    return;
  }

  auto dataCenterInfo = findModuleInfo(moduleInfos, kDataCenterModuleName);
  if (needsDataCenter && !dataCenterInfo) {
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
  std::optional<ModuleManagerProto::ModuleInfo> dlt645Info;
  std::optional<ModuleManagerProto::ModuleInfo> mqttInfo;
  if (hasDlt645) {
    dlt645Info = findModuleInfo(moduleInfos, kDlt645ModuleName);
    if (!dlt645Info) {
      LOG_ERROR("未找到模块: {}", kDlt645ModuleName);
      return;
    }
    mqttInfo = findModuleInfo(moduleInfos, kMqttManagerModuleName);
    if (!mqttInfo) {
      LOG_ERROR("未找到模块: {}", kMqttManagerModuleName);
      return;
    }
  }
  std::optional<ModuleManagerProto::ModuleInfo> comMockInfo;
  if (hasComMock) {
    comMockInfo = findModuleInfo(moduleInfos, kComMockModuleName);
    if (!comMockInfo) {
      LOG_ERROR("未找到模块: {}", kComMockModuleName);
    }
  }

  ModuleManagerProto::ModuleRunningInfos running;
  if (!fetchRunningModuleInfos(moduleStub.get(), &running)) {
    LOG_ERROR("获取运行中模块信息失败，终止下发");
    return;
  }

  auto runningDataCenter = findRunningInfo(running, kDataCenterModuleName);
  if (needsDataCenter && !runningDataCenter) {
    LOG_INFO("DataCenter 未运行，开始启动");
    if (!startModule(moduleStub.get(), *dataCenterInfo)) {
      LOG_ERROR("启动模块 {} 失败", kDataCenterModuleName);
      return;
    }
    runningDataCenter = waitForModule(moduleStub.get(), kDataCenterModuleName, kModuleStartTimeout);
    if (!runningDataCenter) {
      LOG_ERROR("等待 DataCenter 启动超时");
      return;
    }
    LOG_INFO("DataCenter 已启动");
  } else if (needsDataCenter) {
    LOG_INFO("DataCenter 已在运行");
  }

  std::optional<ModuleManagerProto::ModuleRunningInfo> runningIec104;
  if (hasIec104) {
    runningIec104 = findRunningInfo(running, kIec104ModuleName);
    if (!runningIec104) {
      LOG_INFO("IEC104 未运行，开始启动");
      if (!startModule(moduleStub.get(), *iec104Info)) {
        LOG_ERROR("启动模块 {} 失败", kIec104ModuleName);
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
      if (!startModule(moduleStub.get(), *modbusInfo)) {
        LOG_ERROR("启动模块 {} 失败", kModbusRtuModuleName);
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

  std::optional<ModuleManagerProto::ModuleRunningInfo> runningMqtt;
  if (hasDlt645 && mqttInfo) {
    runningMqtt = findRunningInfo(running, kMqttManagerModuleName);
    if (!runningMqtt) {
      LOG_INFO("MQTTManager 未运行，开始启动");
      if (!startModule(moduleStub.get(), *mqttInfo)) {
        LOG_ERROR("启动模块 {} 失败", kMqttManagerModuleName);
        return;
      }
      runningMqtt = waitForModule(moduleStub.get(), kMqttManagerModuleName, kModuleStartTimeout);
      if (!runningMqtt) {
        LOG_ERROR("等待 MQTTManager 启动超时");
        return;
      }
      LOG_INFO("MQTTManager 已启动");
    } else {
      LOG_INFO("MQTTManager 已在运行");
    }
  }

  std::optional<ModuleManagerProto::ModuleRunningInfo> runningDlt645;
  if (hasDlt645) {
    runningDlt645 = findRunningInfo(running, kDlt645ModuleName);
    if (!runningDlt645) {
      LOG_INFO("DLT645 未运行，开始启动");
      if (!startModule(moduleStub.get(), *dlt645Info)) {
        LOG_ERROR("启动模块 {} 失败", kDlt645ModuleName);
        return;
      }
      runningDlt645 = waitForModule(moduleStub.get(), kDlt645ModuleName, kModuleStartTimeout);
      if (!runningDlt645) {
        LOG_ERROR("等待 DLT645 启动超时");
        return;
      }
      LOG_INFO("DLT645 已启动");
    } else {
      LOG_INFO("DLT645 已在运行");
    }
  }

  std::optional<ModuleManagerProto::ModuleRunningInfo> runningComMock;
  if (hasComMock && comMockInfo) {
    runningComMock = findRunningInfo(running, kComMockModuleName);
    if (!runningComMock) {
      LOG_INFO("COMMock 模块未运行，跳过配置下发");
    } else {
      LOG_INFO("COMMock 模块已在运行");
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

  if (hasComMock && runningComMock) {
    auto comMockChannel = grpc::CreateChannel(runningComMock->inner_grpc_server(), grpc::InsecureChannelCredentials());
    auto comMockStub = COMMockProto::COMMockService::NewStub(comMockChannel);
    if (!applyComMockConfig(comMockConfig->com_mock(), comMockStub.get())) {
      LOG_ERROR("COMMock 配置下发存在错误");
    } else {
      LOG_INFO("COMMock 配置下发完成");
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

  if (hasDlt645 && runningDlt645) {
    auto dltChannel = grpc::CreateChannel(runningDlt645->inner_grpc_server(), grpc::InsecureChannelCredentials());
    auto dltStub = DLT645Proto::DLT645Service::NewStub(dltChannel);
    if (!applyDlt645Config(dlt645Config->dlt645(), dltStub.get())) {
      LOG_ERROR("DLT645 配置下发存在错误");
    } else {
      LOG_INFO("DLT645 配置下发完成");
    }
  }

  if (hasDataCenter && runningDataCenter) {
    auto dataCenterChannel = grpc::CreateChannel(runningDataCenter->inner_grpc_server(), grpc::InsecureChannelCredentials());
    auto dataCenterStub = DataCenterProto::DataCenterService::NewStub(dataCenterChannel);
    if (!ApplyDataCenterConfig(*dataCenterConfig, dataCenterStub.get())) {
      LOG_ERROR("DataCenter 配置下发存在错误");
    } else {
      LOG_INFO("DataCenter 配置下发完成");
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
