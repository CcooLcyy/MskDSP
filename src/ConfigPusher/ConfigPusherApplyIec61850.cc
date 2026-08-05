#include "ConfigPusherApplyIec61850.h"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>

#include "Logger.h"
#include "mskdsp/IEC61850Limits.hpp"

namespace ConfigPusher {
namespace {

constexpr std::uintmax_t kMaxSclFileBytes = 32U * 1024U * 1024U;
constexpr std::uintmax_t kMaxTargetContentBytes = 64U * 1024U * 1024U;
constexpr auto kApplyTargetTimeout = std::chrono::seconds(60);

bool ReadSclFile(const std::filesystem::path& path, std::string* content) {
  if (content == nullptr) {
    return false;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    LOG_ERROR("IEC61850 SCL文件不存在或不是普通文件: 路径={}, 原因={}",
              path.string(), error.message());
    return false;
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    LOG_ERROR("IEC61850读取SCL文件大小失败: 路径={}, 原因={}", path.string(),
              error.message());
    return false;
  }
  if (size == 0 || size > kMaxSclFileBytes) {
    LOG_ERROR("IEC61850 SCL文件大小非法: 路径={}, 字节数={}, 上限={}",
              path.string(), size, kMaxSclFileBytes);
    return false;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    LOG_ERROR("IEC61850打开SCL文件失败: 路径={}", path.string());
    return false;
  }
  content->assign(std::istreambuf_iterator<char>(stream),
                  std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof()) {
    LOG_ERROR("IEC61850读取SCL文件失败: 路径={}", path.string());
    return false;
  }
  return true;
}

std::filesystem::path ResolveSclPath(
    const std::filesystem::path& configFilePath,
    const std::string& configuredPath) {
  std::filesystem::path path(configuredPath);
  if (path.is_relative()) {
    path = configFilePath.parent_path() / path;
  }
  return path.lexically_normal();
}

bool BuildRequest(const ConfigPusherProto::Iec61850Config& config,
                  const std::filesystem::path& configFilePath,
                  IEC61850Proto::ApplyTargetConfigRequest* request) {
  if (request == nullptr) {
    return false;
  }
  request->Clear();
  std::unordered_set<std::string> modelNames;
  std::uintmax_t totalContentBytes = 0;
  for (const auto& task : config.models()) {
    if (task.model_name().empty() || task.scl_file().empty()) {
      LOG_ERROR("IEC61850模型任务必须配置model_name和scl_file");
      return false;
    }
    if (!modelNames.emplace(task.model_name()).second) {
      LOG_ERROR("IEC61850模型任务名称重复: 模型={}", task.model_name());
      return false;
    }
    const auto path = ResolveSclPath(configFilePath, task.scl_file());
    std::string content;
    if (!ReadSclFile(path, &content)) {
      return false;
    }
    if (content.size() > kMaxTargetContentBytes - totalContentBytes) {
      LOG_ERROR("IEC61850完整目标态SCL内容总量超过上限: 当前字节数={}, 新增字节数={}, 上限={}",
                totalContentBytes, content.size(), kMaxTargetContentBytes);
      return false;
    }
    totalContentBytes += content.size();
    auto* target = request->add_models();
    target->set_model_name(task.model_name());
    target->set_source_name(task.source_name().empty()
                                ? path.filename().string()
                                : task.source_name());
    target->set_content(std::move(content));
    LOG_INFO("IEC61850已读取SCL目标文件: 模型={}, 来源={}, 路径={}, 字节数={}",
             target->model_name(), target->source_name(), path.string(),
             target->content().size());
  }

  std::unordered_set<std::string> connNames;
  for (const auto& target : config.ieds()) {
    const auto& connName = target.config().conn_name();
    if (connName.empty()) {
      LOG_ERROR("IEC61850 IED目标缺少config.conn_name");
      return false;
    }
    if (!connNames.emplace(connName).second) {
      LOG_ERROR("IEC61850 IED目标连接名重复: IED={}", connName);
      return false;
    }
    if (!modelNames.contains(target.config().model_name())) {
      LOG_ERROR("IEC61850 IED目标引用未声明模型: IED={}, 模型={}", connName,
                target.config().model_name());
      return false;
    }
    *request->add_ieds() = target;
  }
  return true;
}

}  // namespace

bool applyIec61850Config(
    const ConfigPusherProto::Iec61850Config& config,
    const std::filesystem::path& configFilePath,
    IEC61850Proto::IEC61850Service::StubInterface* stub) {
  return applyIec61850Config(
      config, configFilePath, stub,
      static_cast<std::size_t>(mskdsp::kIec61850MaxGrpcMessageBytes));
}

bool applyIec61850Config(
    const ConfigPusherProto::Iec61850Config& config,
    const std::filesystem::path& configFilePath,
    IEC61850Proto::IEC61850Service::StubInterface* stub,
    std::size_t maxSerializedRequestBytes) {
  if (stub == nullptr) {
    LOG_ERROR("IEC61850 gRPC stub为空");
    return false;
  }
  IEC61850Proto::ApplyTargetConfigRequest request;
  if (!BuildRequest(config, configFilePath, &request)) {
    return false;
  }
  const auto serializedSize = request.ByteSizeLong();
  if (serializedSize > maxSerializedRequestBytes) {
    LOG_ERROR("IEC61850完整目标态序列化大小超过上限: 字节数={}, 上限={}",
              serializedSize, maxSerializedRequestBytes);
    return false;
  }
  LOG_INFO("开始下发IEC61850完整目标态: 模型数={}, IED数={}",
           request.models_size(), request.ieds_size());
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kApplyTargetTimeout);
  IEC61850Proto::ApplyTargetConfigResponse response;
  const auto status = stub->ApplyTargetConfig(&context, request, &response);
  if (!status.ok()) {
    LOG_ERROR("IEC61850完整目标态下发失败: 模型数={}, IED数={}, 原因={}",
              request.models_size(), request.ieds_size(), status.error_message());
    return false;
  }
  LOG_INFO("IEC61850完整目标态下发完成: 模型数={}, IED数={}, 校验问题数={}",
           response.models_size(), response.ieds_size(), response.issues_size());
  for (const auto& issue : response.issues()) {
    LOG_WARNING("IEC61850目标态诊断: 级别={}, 编码={}, 路径={}, 原因={}",
                static_cast<int>(issue.severity()), issue.code(), issue.path(),
                issue.message());
  }
  return true;
}

}  // namespace ConfigPusher
