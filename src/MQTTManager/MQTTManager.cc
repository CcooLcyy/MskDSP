#include "MQTTManager.h"

#include <boost/dll.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
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
#include <thread>
#include <unordered_map>

#include <lua.hpp>

#include "Logger.h"
#include "MQTTManagerGrpcService.h"
#include "MQTTManagerLibInfo.h"
#include "ModuleManager.pb.h"

namespace {
constexpr size_t kScriptPreviewLen = 256;
constexpr size_t kMapValuePreviewLen = 128;
constexpr const char* kDefaultDecodeEntry = "decode";
constexpr const char* kDefaultEncodeEntry = "encode";

std::string escapeForLog(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) {
    switch (ch) {
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::string formatPreview(const std::string& text, size_t maxLen) {
  const auto escaped = escapeForLog(text);
  if (escaped.size() <= maxLen) {
    return escaped;
  }
  return escaped.substr(0, maxLen) + "...";
}

std::string formatDataMap(const google::protobuf::Map<std::string, std::string>& data) {
  if (data.empty()) {
    return "{}";
  }
  std::ostringstream oss;
  oss << "{";
  bool first = true;
  for (const auto& item : data) {
    if (!first) {
      oss << ", ";
    }
    first = false;
    oss << item.first << "=" << formatPreview(item.second, kMapValuePreviewLen);
  }
  oss << "}";
  return oss.str();
}

std::string formatTopicFilters(const google::protobuf::RepeatedPtrField<MQTTManagerProto::TopicFilter>& topics) {
  if (topics.empty()) {
    return "[]";
  }
  std::ostringstream oss;
  oss << "[";
  for (int i = 0; i < topics.size(); ++i) {
    if (i > 0) {
      oss << ", ";
    }
    const auto& filter = topics.at(i);
    oss << filter.topic() << "(质量等级=" << filter.qos() << ")";
  }
  oss << "]";
  return oss.str();
}

std::filesystem::path resolveScriptBaseDir() {
  try {
    std::filesystem::path exePath(boost::dll::program_location().string());
    return (exePath.parent_path() / "conf" / "MQTTManager" / "script").lexically_normal();
  } catch (const std::exception& ex) {
    LOG_WARNING("MQTTManager 获取可执行文件路径失败，使用当前目录: {}", ex.what());
  }
  return (std::filesystem::current_path() / "conf" / "MQTTManager" / "script").lexically_normal();
}

std::optional<std::filesystem::path> resolveScriptPath(const std::filesystem::path& baseDir, const std::string& filePath,
                                                       std::string* error) {
  if (filePath.empty()) {
    if (error != nullptr) {
      *error = "脚本路径为空";
    }
    return std::nullopt;
  }
  std::filesystem::path relative(filePath);
  if (relative.is_absolute()) {
    if (error != nullptr) {
      *error = "脚本路径必须为相对路径";
    }
    return std::nullopt;
  }
  for (const auto& part : relative) {
    if (part == "..") {
      if (error != nullptr) {
        *error = "脚本路径不允许包含 ..";
      }
      return std::nullopt;
    }
  }
  return (baseDir / relative).lexically_normal();
}

bool readFile(const std::filesystem::path& path, std::string* out) {
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

bool compileLuaScript(const std::string& script, std::string* outError) {
  std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), &lua_close);
  if (!state) {
    if (outError != nullptr) {
      *outError = "Lua 状态创建失败";
    }
    return false;
  }
  luaL_requiref(state.get(), "_G", luaopen_base, 1);
  luaL_requiref(state.get(), LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(state.get(), 2);
  const int rc = luaL_loadstring(state.get(), script.c_str());
  if (rc != LUA_OK) {
    const char* err = lua_tostring(state.get(), -1);
    if (outError != nullptr) {
      *outError = err ? std::string("脚本编译失败: ") + err : "脚本编译失败";
    }
    return false;
  }
  return true;
}

std::string decodeEntryOrDefault(const MQTTManagerProto::ScriptConfig& script) {
  if (!script.decode_entry().empty()) {
    return script.decode_entry();
  }
  return kDefaultDecodeEntry;
}

std::string encodeEntryOrDefault(const MQTTManagerProto::ScriptConfig& script) {
  if (!script.encode_entry().empty()) {
    return script.encode_entry();
  }
  return kDefaultEncodeEntry;
}

const std::string& GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(MQTTManagerLibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(MQTTManagerLibInfo.VERSION_MAJOR);
    version->set_minor(MQTTManagerLibInfo.VERSION_MINOR);
    version->set_patch(MQTTManagerLibInfo.VERSION_PATCH);
    version->set_version(MQTTManagerLibInfo.VERSION);
    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace MQTTManager {
MQTTManager::MQTTManager() :
  ModuleInterface(),
  mQTTManagerService_(std::make_shared<MQTTManagerGrpcServiceImpl>()) {
  initLibInfo(MQTTManagerLibInfo);
}

MQTTManager::~MQTTManager() {}

void MQTTManager::start(std::stop_token stopToken) {
  LOG_INFO("MQTTManager 模块启动");
  mQTTManagerService_->setMQTTManager(this);
  LOG_INFO("MQTTManager 服务实例绑定完成");
  grpcServerBuilder(mQTTManagerService_);
  LOG_INFO("MQTTManager gRPC 服务已启动");

  std::mutex mu;
  std::condition_variable_any cv;
  std::stop_callback cb(stopToken, [&cv]() { cv.notify_all(); });
  std::unique_lock lock(mu);
  cv.wait(lock, [&stopToken]() { return stopToken.stop_requested(); });
  LOG_INFO("MQTTManager 模块停止");
}

grpc::Status MQTTManager::UpdateConfig(const MQTTManagerProto::UpdateConfigRequest& request,
                                       MQTTManagerProto::UpdateConfigResponse* response) {
  if (response == nullptr) {
    LOG_ERROR("MQTTManager 配置更新响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  LOG_INFO("MQTTManager 收到配置下发: 配置数量={}", request.profiles_size());
  if (request.profiles_size() == 0) {
    response->set_ok(false);
    response->set_message("配置为空");
    LOG_ERROR("MQTTManager 配置下发失败: 配置为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "配置为空");
  }

  std::unordered_map<std::string, ProfileRuntime> newProfiles;
  const auto baseDir = resolveScriptBaseDir();
  LOG_INFO("MQTTManager 脚本基础目录: {}", baseDir.string());

  for (const auto& profile : request.profiles()) {
    if (profile.profile_id().empty()) {
      response->set_ok(false);
      response->set_message("配置标识不能为空");
      LOG_ERROR("MQTTManager 配置下发失败: 配置标识为空");
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "配置标识不能为空");
    }
    if (newProfiles.contains(profile.profile_id())) {
      response->set_ok(false);
      response->set_message("配置标识重复");
      LOG_ERROR("MQTTManager 配置下发失败: 配置标识重复, 标识={}", profile.profile_id());
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "配置标识重复");
    }

    const auto& conn = profile.connection();
    LOG_INFO("MQTTManager 连接配置: 标识={}, 代理地址={}, 客户端标识={}, 用户名={}, 密码长度={}, 保活秒数={}, 清理会话={}",
             profile.profile_id(), conn.broker_uri(), conn.client_id(), conn.username(), conn.password().size(),
             conn.keepalive_sec(), conn.clean_session());

    ProfileRuntime runtime;
    runtime.config = profile;
    runtime.script.decodeEntry = decodeEntryOrDefault(profile.script());
    runtime.script.encodeEntry = encodeEntryOrDefault(profile.script());

    std::string scriptSource;
    std::string scriptText;
    if (!profile.script().inline_script().empty()) {
      scriptText = profile.script().inline_script();
      scriptSource = "内联";
      LOG_INFO("MQTTManager 脚本来源=内联: 标识={}, 长度={}, 预览={}", profile.profile_id(),
               scriptText.size(), formatPreview(scriptText, kScriptPreviewLen));
    } else if (!profile.script().file_path().empty()) {
      std::string error;
      auto resolved = resolveScriptPath(baseDir, profile.script().file_path(), &error);
      if (!resolved.has_value()) {
        response->set_ok(false);
        response->set_message(error);
        LOG_ERROR("MQTTManager 配置下发失败: 标识={}, 原因={}", profile.profile_id(), error);
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error);
      }
      if (!readFile(*resolved, &scriptText)) {
        response->set_ok(false);
        response->set_message("读取脚本文件失败");
        LOG_ERROR("MQTTManager 配置下发失败: 标识={}, 脚本读取失败, 路径={}", profile.profile_id(), resolved->string());
        return grpc::Status(grpc::StatusCode::INTERNAL, "读取脚本文件失败");
      }
      scriptSource = "文件:" + resolved->string();
      LOG_INFO("MQTTManager 脚本来源=文件: 标识={}, 路径={}, 长度={}, 预览={}", profile.profile_id(),
               resolved->string(), scriptText.size(), formatPreview(scriptText, kScriptPreviewLen));
    } else {
      response->set_ok(false);
      response->set_message("脚本未配置");
      LOG_ERROR("MQTTManager 配置下发失败: 标识={}, 未配置脚本", profile.profile_id());
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "脚本未配置");
    }

    std::string compileError;
    if (!compileLuaScript(scriptText, &compileError)) {
      response->set_ok(false);
      response->set_message("脚本编译失败");
      LOG_ERROR("MQTTManager 配置下发失败: 标识={}, 原因={}", profile.profile_id(), compileError);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "脚本编译失败");
    }

    runtime.script.source = scriptSource;
    runtime.script.script = scriptText;
    runtime.script.lastError.clear();
    runtime.ready = true;

    newProfiles.emplace(profile.profile_id(), std::move(runtime));
    LOG_INFO("MQTTManager 配置解析完成: 标识={}, 解码入口={}, 编码入口={}", profile.profile_id(),
             decodeEntryOrDefault(profile.script()), encodeEntryOrDefault(profile.script()));
  }

  {
    std::lock_guard<std::mutex> lock(configMutex_);
    profiles_ = std::move(newProfiles);
    hasConfig_ = true;
    lastConfigMessage_ = "配置更新成功";
  }

  response->set_ok(true);
  response->set_message("配置更新成功");
  LOG_INFO("MQTTManager 配置更新响应: 成功={}, 消息={}", response->ok(), response->message());
  LOG_INFO("MQTTManager 配置更新完成: 配置数量={}", request.profiles_size());
  return grpc::Status::OK;
}

grpc::Status MQTTManager::Publish(const MQTTManagerProto::PublishRequest& request,
                                  MQTTManagerProto::PublishResponse* response) {
  if (response == nullptr) {
    LOG_ERROR("MQTTManager 发布响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  LOG_INFO("MQTTManager 收到发布请求: 配置标识={}, 主题={}, 质量等级(qos)={}, 保留标志(retain)={}, 数据={}", request.profile_id(),
           request.topic(), request.qos(), request.retain(), formatDataMap(request.data()));
  if (request.profile_id().empty() || request.topic().empty()) {
    response->set_ok(false);
    response->set_message("配置标识或主题为空");
    LOG_ERROR("MQTTManager 发布失败: 配置标识或主题为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "配置标识或主题为空");
  }
  if (request.data().empty()) {
    response->set_ok(false);
    response->set_message("字段数据为空");
    LOG_ERROR("MQTTManager 发布失败: 字段数据为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "字段数据为空");
  }

  {
    std::lock_guard<std::mutex> lock(configMutex_);
    if (!hasConfig_) {
      response->set_ok(false);
      response->set_message("尚未加载配置");
      LOG_ERROR("MQTTManager 发布失败: 尚未加载配置");
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "尚未加载配置");
    }
    if (!profiles_.contains(request.profile_id())) {
      response->set_ok(false);
      response->set_message("未找到配置标识");
      LOG_ERROR("MQTTManager 发布失败: 未找到配置标识, 标识={}", request.profile_id());
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "未找到配置标识");
    }
  }

  response->set_ok(false);
  response->set_message("MQTT 发布尚未启用");
  LOG_WARNING("MQTTManager 发布尚未启用: 配置标识={}, 主题={}", request.profile_id(), request.topic());
  LOG_INFO("MQTTManager 发布响应: 成功={}, 消息={}", response->ok(), response->message());
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "MQTT 发布尚未启用");
}

grpc::Status MQTTManager::Subscribe(const MQTTManagerProto::SubscribeRequest& request,
                                    grpc::ServerWriter<MQTTManagerProto::SubscribeResponse>* writer) {
  if (writer == nullptr) {
    LOG_ERROR("MQTTManager 订阅响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  LOG_INFO("MQTTManager 收到订阅请求: 配置标识={}, 主题过滤={}", request.profile_id(),
           formatTopicFilters(request.topics()));
  if (request.profile_id().empty()) {
    LOG_ERROR("MQTTManager 订阅失败: 配置标识为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "配置标识为空");
  }
  if (request.topics().empty()) {
    LOG_ERROR("MQTTManager 订阅失败: 订阅主题为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "订阅主题为空");
  }
  for (const auto& filter : request.topics()) {
    if (filter.topic().empty()) {
      LOG_ERROR("MQTTManager 订阅失败: 订阅主题为空");
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "订阅主题为空");
    }
  }

  {
    std::lock_guard<std::mutex> lock(configMutex_);
    if (!hasConfig_) {
      LOG_ERROR("MQTTManager 订阅失败: 尚未加载配置");
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "尚未加载配置");
    }
    if (!profiles_.contains(request.profile_id())) {
      LOG_ERROR("MQTTManager 订阅失败: 未找到配置标识, 标识={}", request.profile_id());
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "未找到配置标识");
    }
  }

  LOG_WARNING("MQTTManager 订阅尚未启用: 配置标识={}", request.profile_id());
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "MQTT 订阅尚未启用");
}

grpc::Status MQTTManager::GetStatus(const MQTTManagerProto::GetStatusRequest&,
                                    MQTTManagerProto::GetStatusResponse* response) {
  if (response == nullptr) {
    LOG_ERROR("MQTTManager 状态查询响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }

  std::lock_guard<std::mutex> lock(configMutex_);
  if (!hasConfig_) {
    response->set_ok(false);
    response->set_message("尚未加载配置");
    LOG_WARNING("MQTTManager 状态查询: 尚未加载配置");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "尚未加载配置");
  }

  response->set_ok(true);
  if (lastConfigMessage_.empty()) {
    response->set_message("配置已加载");
  } else {
    response->set_message(lastConfigMessage_);
  }

  for (const auto& [profileId, runtime] : profiles_) {
    auto* status = response->add_profiles();
    status->set_profile_id(profileId);
    status->set_config_ready(runtime.ready);
    status->set_script_source(runtime.script.source);
    status->set_decode_entry(runtime.script.decodeEntry);
    status->set_encode_entry(runtime.script.encodeEntry);
    status->set_last_error(runtime.script.lastError);
    LOG_INFO("MQTTManager 状态详情: 标识={}, 就绪={}, 脚本来源={}, 解码入口={}, 编码入口={}, 最近错误={}",
             profileId, runtime.ready, runtime.script.source, runtime.script.decodeEntry, runtime.script.encodeEntry,
             runtime.script.lastError);
  }
  LOG_INFO("MQTTManager 状态响应: 消息={}, 配置数量={}", response->message(), response->profiles_size());
  return grpc::Status::OK;
}
}  // namespace MQTTManager

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new MQTTManager::MQTTManager();
}

extern "C" BOOST_SYMBOL_EXPORT bool GetModuleManifestPb(const uint8_t** data, size_t* size) {
  if (data == nullptr || size == nullptr) {
    return false;
  }
  const auto& serialized = GetSerializedManifest();
  *data = reinterpret_cast<const uint8_t*>(serialized.data());
  *size = serialized.size();
  return true;
}
