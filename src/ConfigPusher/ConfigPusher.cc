#include "ConfigPusher.h"

#include <google/protobuf/util/json_util.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <boost/dll.hpp>
#include <chrono>
#include <condition_variable>
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
#include "ModuleManager.grpc.pb.h"

namespace ConfigPusher {
namespace {
constexpr const char *kConfigPath = "./conf/configPusher/iec104.jsonc";
constexpr const char *kModuleManagerAddress = "127.0.0.1:7000";
constexpr const char *kDataCenterModuleName = "DataCenter";
constexpr const char *kIec104ModuleName = "IEC104";
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
    LOG_ERROR("IEC104 stub is null");
    return false;
  }

  bool ok = true;
  for (const auto &task : config.links()) {
    if (!task.has_link() || !task.link().has_config()) {
      LOG_ERROR("IEC104 link task missing link/config");
      ok = false;
      continue;
    }

    const auto &linkConfig = task.link().config();
    if (linkConfig.conn_name().empty()) {
      LOG_ERROR("IEC104 link task missing config.conn_name");
      ok = false;
      continue;
    }

    IEC104Proto::UpsertLinkRequest linkReq = task.link();
    IEC104Proto::LinkInfo linkInfo;
    grpc::ClientContext linkCtx;
    auto status = stub->UpsertLink(&linkCtx, linkReq, &linkInfo);
    if (!status.ok()) {
      LOG_ERROR("IEC104 UpsertLink failed for {}: {}", linkConfig.conn_name(), status.error_message());
      ok = false;
      continue;
    }

    if (task.has_point_table() && task.point_table().points_size() > 0) {
      IEC104Proto::UpsertPointTableRequest ptReq = task.point_table();
      if (ptReq.conn_name().empty()) {
        ptReq.set_conn_name(linkConfig.conn_name());
      }
      grpc::ClientContext ptCtx;
      IEC104Proto::Empty ptResp;
      status = stub->UpsertPointTable(&ptCtx, ptReq, &ptResp);
      if (!status.ok()) {
        LOG_ERROR("IEC104 UpsertPointTable failed for {}: {}", ptReq.conn_name(), status.error_message());
        ok = false;
        continue;
      }
    }

    if (task.start()) {
      IEC104Proto::StartLinkRequest startReq;
      startReq.set_conn_name(linkConfig.conn_name());
      grpc::ClientContext startCtx;
      IEC104Proto::Empty startResp;
      status = stub->StartLink(&startCtx, startReq, &startResp);
      if (!status.ok()) {
        LOG_ERROR("IEC104 StartLink failed for {}: {}", linkConfig.conn_name(), status.error_message());
        ok = false;
        continue;
      }
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
  configPusherService_->getConfigPusher(this);
  grpcServerBuilder(configPusherService_);
  applyConfig();

  std::mutex mu;
  std::condition_variable_any cv;
  std::stop_callback cb(stopToken, [&cv]() { cv.notify_all(); });
  std::unique_lock lock(mu);
  cv.wait(lock, [&stopToken]() { return stopToken.stop_requested(); });
}

void ConfigPusher::applyConfig() {
  if (!std::filesystem::exists(kConfigPath)) {
    LOG_INFO("ConfigPusher config not found: {}", kConfigPath);
    return;
  }

  std::string raw;
  if (!readFile(kConfigPath, &raw)) {
    LOG_ERROR("ConfigPusher failed to read config: {}", kConfigPath);
    return;
  }

  auto json = stripJsonComments(raw);
  ConfigPusherProto::Config config;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = false;
  auto parseStatus = google::protobuf::util::JsonStringToMessage(json, &config, options);
  if (!parseStatus.ok()) {
    LOG_ERROR("ConfigPusher config parse failed: {}", parseStatus.ToString());
    return;
  }

  if (!config.has_iec104() || config.iec104().links().empty()) {
    LOG_INFO("ConfigPusher config has no IEC104 links");
    return;
  }

  auto channel = grpc::CreateChannel(kModuleManagerAddress, grpc::InsecureChannelCredentials());
  auto moduleStub = ModuleManagerProto::ModuleManage::NewStub(channel);

  ModuleManagerProto::ModuleInfos moduleInfos;
  grpc::ClientContext infoCtx;
  ModuleManagerProto::Empty infoReq;
  auto status = moduleStub->GetModuleInfo(&infoCtx, infoReq, &moduleInfos);
  if (!status.ok()) {
    LOG_ERROR("ConfigPusher GetModuleInfo failed: {}", status.error_message());
    return;
  }

  auto dataCenterInfo = findModuleInfo(moduleInfos, kDataCenterModuleName);
  if (!dataCenterInfo) {
    LOG_ERROR("ConfigPusher cannot find module: {}", kDataCenterModuleName);
    return;
  }
  auto iec104Info = findModuleInfo(moduleInfos, kIec104ModuleName);
  if (!iec104Info) {
    LOG_ERROR("ConfigPusher cannot find module: {}", kIec104ModuleName);
    return;
  }

  ModuleManagerProto::ModuleRunningInfos running;
  grpc::ClientContext runningCtx;
  ModuleManagerProto::Empty runningReq;
  status = moduleStub->GetRunningModuleInfo(&runningCtx, runningReq, &running);
  if (!status.ok()) {
    LOG_ERROR("ConfigPusher GetRunningModuleInfo failed: {}", status.error_message());
    return;
  }

  auto runningDataCenter = findRunningInfo(running, kDataCenterModuleName);
  if (!runningDataCenter) {
    grpc::ClientContext startCtx;
    ModuleManagerProto::Empty startResp;
    status = moduleStub->StartModule(&startCtx, *dataCenterInfo, &startResp);
    if (!status.ok()) {
      LOG_ERROR("ConfigPusher StartModule {} failed: {}", kDataCenterModuleName, status.error_message());
      return;
    }
    runningDataCenter = waitForModule(moduleStub.get(), kDataCenterModuleName, kModuleStartTimeout);
    if (!runningDataCenter) {
      LOG_ERROR("ConfigPusher wait DataCenter running timeout");
      return;
    }
  }

  auto runningIec104 = findRunningInfo(running, kIec104ModuleName);
  if (!runningIec104) {
    grpc::ClientContext startCtx;
    ModuleManagerProto::Empty startResp;
    status = moduleStub->StartModule(&startCtx, *iec104Info, &startResp);
    if (!status.ok()) {
      LOG_ERROR("ConfigPusher StartModule {} failed: {}", kIec104ModuleName, status.error_message());
      return;
    }
    runningIec104 = waitForModule(moduleStub.get(), kIec104ModuleName, kModuleStartTimeout);
    if (!runningIec104) {
      LOG_ERROR("ConfigPusher wait IEC104 running timeout");
      return;
    }
  }

  auto iecChannel = grpc::CreateChannel(runningIec104->inner_grpc_server(), grpc::InsecureChannelCredentials());
  auto iecStub = IEC104Proto::IEC104Service::NewStub(iecChannel);

  if (!applyIec104Config(config.iec104(), iecStub.get())) {
    LOG_ERROR("ConfigPusher IEC104 apply finished with errors");
  } else {
    LOG_INFO("ConfigPusher IEC104 apply completed");
  }
}
}  // namespace ConfigPusher

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface *create() {
  return new ConfigPusher::ConfigPusher();
}
