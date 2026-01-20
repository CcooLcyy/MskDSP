#include "ConfigPusherModuleManager.h"

#include <google/protobuf/message.h>

#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include "Logger.h"

namespace ConfigPusher {
namespace {
constexpr auto kModulePollInterval = std::chrono::milliseconds(200);

std::string formatProtoForLog(const google::protobuf::Message &message) {
#ifdef MSKDSP_TEST_DISABLE_PROTO_LOG
  return "测试环境跳过报文内容";
#else
  auto text = message.ShortDebugString();
  if (text.empty()) {
    return "空";
  }
  return text;
#endif
}
}  // namespace

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

bool fetchModuleInfos(ModuleManagerProto::ModuleManage::StubInterface *stub,
                      ModuleManagerProto::ModuleInfos *out) {
  if (stub == nullptr || out == nullptr) {
    LOG_ERROR("ModuleManager 获取模块信息入参为空");
    return false;
  }
  ModuleManagerProto::Empty req;
  LOG_INFO("发送 ModuleManager 获取模块信息请求: {}", formatProtoForLog(req));
  grpc::ClientContext ctx;
  auto status = stub->GetModuleInfo(&ctx, req, out);
  if (!status.ok()) {
    LOG_ERROR("获取模块信息失败: {}，请求={}", status.error_message(), formatProtoForLog(req));
    return false;
  }
  LOG_INFO("收到 ModuleManager 获取模块信息响应: {}", formatProtoForLog(*out));
  return true;
}

bool fetchRunningModuleInfos(ModuleManagerProto::ModuleManage::StubInterface *stub,
                             ModuleManagerProto::ModuleRunningInfos *out) {
  if (stub == nullptr || out == nullptr) {
    LOG_ERROR("ModuleManager 获取运行中模块信息入参为空");
    return false;
  }
  ModuleManagerProto::Empty req;
  LOG_INFO("发送 ModuleManager 获取运行中模块信息请求: {}", formatProtoForLog(req));
  grpc::ClientContext ctx;
  auto status = stub->GetRunningModuleInfo(&ctx, req, out);
  if (!status.ok()) {
    LOG_ERROR("获取运行中模块信息失败: {}，请求={}", status.error_message(), formatProtoForLog(req));
    return false;
  }
  LOG_INFO("收到 ModuleManager 获取运行中模块信息响应: {}", formatProtoForLog(*out));
  return true;
}

bool startModule(ModuleManagerProto::ModuleManage::StubInterface *stub,
                 const ModuleManagerProto::ModuleInfo &info) {
  if (stub == nullptr) {
    LOG_ERROR("ModuleManager gRPC stub 为空");
    return false;
  }
  LOG_INFO("发送 ModuleManager 启动模块请求: {}", formatProtoForLog(info));
  grpc::ClientContext ctx;
  ModuleManagerProto::Empty resp;
  auto status = stub->StartModule(&ctx, info, &resp);
  if (!status.ok()) {
    LOG_ERROR("启动模块失败: {}，请求={}", status.error_message(), formatProtoForLog(info));
    return false;
  }
  LOG_INFO("收到 ModuleManager 启动模块响应: {}", formatProtoForLog(resp));
  return true;
}

std::optional<ModuleManagerProto::ModuleRunningInfo> waitForModule(
    ModuleManagerProto::ModuleManage::StubInterface *stub,
    std::string_view moduleName,
    std::chrono::milliseconds timeout) {
  if (stub == nullptr) {
    LOG_ERROR("ModuleManager 等待模块运行入参为空");
    return std::nullopt;
  }

  LOG_INFO("开始等待模块运行: 模块名={}, 超时毫秒={}", moduleName, timeout.count());
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    ModuleManagerProto::ModuleRunningInfos running;
    if (!fetchRunningModuleInfos(stub, &running)) {
      return std::nullopt;
    }
    auto found = findRunningInfo(running, moduleName);
    if (found) {
      LOG_INFO("等待模块运行完成: 模块名={}, 运行信息={}", moduleName, formatProtoForLog(*found));
      return found;
    }
    std::this_thread::sleep_for(kModulePollInterval);
  }
  LOG_ERROR("等待模块运行超时: 模块名={}", moduleName);
  return std::nullopt;
}
}  // namespace ConfigPusher
