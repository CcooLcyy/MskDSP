#include "ModuleInterface.h"

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/support/server_interceptor.h>

#include <algorithm>
#include <boost/json.hpp>
#include <filesystem>
#include <format>
#include <functional>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Logger.h"
#include "ThreadUtil.hpp"

namespace {
std::string toHex(std::string_view text) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (unsigned char c : text) {
    oss << std::setw(2) << static_cast<int>(c);
  }
  return oss.str();
}

void logJsonFieldDetails(std::string_view title, const boost::json::object &obj, std::string_view phase, std::string_view moduleName) {
  LOG_INFO("{}字段数量={}，阶段: {}，模块: {}", title, obj.size(), phase, moduleName);
  if (obj.empty()) {
    return;
  }
  for (const auto &entry : obj) {
    const auto &key = entry.key();
    std::string keyText(key.data(), key.size());
    LOG_INFO("{}字段明细: 名称='{}'，字节数={}，十六进制={}，阶段: {}，模块: {}", title, keyText, keyText.size(), toHex(keyText), phase, moduleName);
  }
}

std::string sanitizeJsonSnippet(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
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

void logJsonParseError(std::string_view title, std::string_view phase, std::string_view moduleName, std::string_view json, std::size_t offset, int errorValue, bool warnOnly) {
  const std::size_t safeOffset = std::min(offset, json.size());
  std::size_t line = 1;
  std::size_t column = 1;
  for (std::size_t i = 0; i < safeOffset; ++i) {
    const char ch = json[i];
    if (ch == '\n') {
      ++line;
      column = 1;
    } else if (ch == '\r') {
      ++line;
      column = 1;
      if (i + 1 < safeOffset && json[i + 1] == '\n') {
        ++i;
      }
    } else {
      ++column;
    }
  }
  constexpr std::size_t kSnippetRadius = 20;
  const std::size_t snippetStart = safeOffset > kSnippetRadius ? safeOffset - kSnippetRadius : 0;
  const std::size_t snippetEnd = std::min(json.size(), safeOffset + kSnippetRadius);
  const auto snippet = sanitizeJsonSnippet(json.substr(snippetStart, snippetEnd - snippetStart));
  if (warnOnly) {
    LOG_WARNING("{}解析失败: JSON 语法错误码={}，行={}，列={}，偏移={}，附近文本='{}'，阶段: {}，模块: {}", title, errorValue, line, column, safeOffset, snippet, phase, moduleName);
  } else {
    LOG_ERROR("{}解析失败: JSON 语法错误码={}，行={}，列={}，偏移={}，附近文本='{}'，阶段: {}，模块: {}", title, errorValue, line, column, safeOffset, snippet, phase, moduleName);
  }
}

bool parseJsonValue(std::string_view json, boost::json::value *out, std::string_view title, std::string_view phase, std::string_view moduleName, bool warnOnly) {
  if (out == nullptr) {
    logJsonParseError(title, phase, moduleName, json, 0, -1, warnOnly);
    return false;
  }
  boost::json::parser parser;
  boost::system::error_code ec;
  const std::size_t consumed = parser.write(json, ec);
  if (ec) {
    logJsonParseError(title, phase, moduleName, json, consumed, ec.value(), warnOnly);
    return false;
  }
  *out = parser.release();
  return true;
}

bool selfCheckGrpcConfig(std::string_view phase, std::string_view moduleName, bool logOnSuccess) {
  static const std::string kSelfCheckJson = R"({"auto_start_modules":["ConfigPusher"]})";
  boost::json::value parsed;
  if (!parseJsonValue(kSelfCheckJson, &parsed, "gRPC 自检", phase, moduleName, true)) {
    return false;
  }
  if (!parsed.is_object()) {
    LOG_WARNING("gRPC 自检结果不是对象，阶段: {}，模块: {}", phase, moduleName);
    return false;
  }
  const auto &obj = parsed.as_object();
  auto fieldIt = obj.find("auto_start_modules");
  if (fieldIt == obj.end()) {
    LOG_WARNING("gRPC 自检未找到字段 auto_start_modules，阶段: {}，模块: {}", phase, moduleName);
    logJsonFieldDetails("gRPC 自检", obj, phase, moduleName);
    return false;
  }
  if (!fieldIt->value().is_array()) {
    LOG_WARNING("gRPC 自检字段类型异常，阶段: {}，模块: {}", phase, moduleName);
    logJsonFieldDetails("gRPC 自检", obj, phase, moduleName);
    return false;
  }
  if (logOnSuccess) {
    LOG_INFO("gRPC 自检通过，阶段: {}，模块: {}", phase, moduleName);
  }
  return true;
}

class ModuleLogInterceptor final : public grpc::experimental::Interceptor {
public:
  explicit ModuleLogInterceptor(std::string moduleName) :
    moduleName_(std::move(moduleName)) {
  }

  void Intercept(grpc::experimental::InterceptorBatchMethods *methods) override {
    if (methods == nullptr) {
      return;
    }

    if (!moduleScope_) {
      moduleScope_ = std::make_unique<ModuleManager::LogModuleScope>(moduleName_);
    }

    if (methods->QueryInterceptionHookPoint(grpc::experimental::InterceptionHookPoints::POST_RECV_CLOSE)) {
      moduleScope_.reset();
    }

    methods->Proceed();
  }

private:
  std::string moduleName_;
  std::unique_ptr<ModuleManager::LogModuleScope> moduleScope_;
};

class ModuleLogInterceptorFactory final : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
  explicit ModuleLogInterceptorFactory(std::string moduleName) :
    moduleName_(std::move(moduleName)) {
  }

  grpc::experimental::Interceptor *CreateServerInterceptor(grpc::experimental::ServerRpcInfo *) override {
    return new ModuleLogInterceptor(moduleName_);
  }

private:
  std::string moduleName_;
};

std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> createInterceptorCreators(const std::string &moduleName) {
  std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> creators;
  creators.emplace_back(std::make_unique<ModuleLogInterceptorFactory>(moduleName));
  return creators;
}
}  // namespace

namespace ModuleInterface {
std::set<std::string> ModuleInterface::allocatedPorts_;
std::mutex ModuleInterface::portMutex_;
ModuleInterface::ModuleInterface() {
  ModuleManager::Logger::init();
}
ModuleInterface::~ModuleInterface() {
  shutdownServers();
}
MetaData ModuleInterface::metaData() {
  return metaData_;
}
void ModuleInterface::initLibInfo(LibInfo libInfo) {
  metaData_.name = libInfo.LIB_NAME;
  metaData_.libName = std::format("{}{}{}.{}", "lib", libInfo.LIB_NAME, ".so", libInfo.VERSION);
  ::ModuleInterface::Version versionInfo{
      libInfo.VERSION_MAJOR,
      libInfo.VERSION_MINOR,
      libInfo.VERSION_PATCH,
      libInfo.VERSION};
  metaData_.version = versionInfo;
  std::string socktPath{std::format("./socket/{}.sock", metaData_.name)};
  std::filesystem::path path(socktPath);
  if (!std::filesystem::exists(path.parent_path())) {
    std::filesystem::create_directory(path.parent_path());
  }
  auto absPath = std::filesystem::canonical(path.parent_path());
  auto absFilePath = std::format("{}/{}.sock", absPath.c_str(), metaData_.name);
  auto sockPath = std::format("unix:{}", absFilePath);
  if (std::filesystem::exists(absFilePath)) {
    std::filesystem::remove(absFilePath);
  }
  metaData_.innerGRPCServer = sockPath;

  auto port = getRandomPort();
  metaData_.outerGRPCServer = std::format("0.0.0.0:{}", port);
}
void ModuleInterface::grpcServerBuilder(std::shared_ptr<grpc::Service> service) {
  selfCheckGrpcConfig("gRPC 构建前", metaData_.name, true);
  grpc::ServerBuilder serverBuilder;
  serverBuilder.RegisterService(service.get());
  LOG_INFO("gRPC 反射已移除，跳过反射初始化");
  LOG_INFO("gRPC 使用单一服务监听内外端口");
  serverBuilder.experimental().SetInterceptorCreators(createInterceptorCreators(metaData_.name));
  int innerPort = 0;
  int outerPort = 0;
  serverBuilder.AddListeningPort(metaData_.innerGRPCServer, grpc::InsecureServerCredentials(), &innerPort);
  serverBuilder.AddListeningPort(metaData_.outerGRPCServer, grpc::InsecureServerCredentials(), &outerPort);
  std::unique_ptr<grpc::Server> serverTmp(serverBuilder.BuildAndStart());
  server_ = std::move(serverTmp);
  if (server_) {
    bool innerReady = true;
    if (metaData_.innerGRPCServer.rfind("unix:", 0) == 0) {
      const auto sockPath = std::filesystem::path(metaData_.innerGRPCServer.substr(5));
      innerReady = std::filesystem::exists(sockPath);
    } else {
      innerReady = innerPort != 0;
    }
    if (innerReady) {
      LOG_INFO("gRPC 内部服务监听成功: {}", metaData_.innerGRPCServer);
    } else {
      LOG_WARNING("gRPC 内部服务监听失败: {}", metaData_.innerGRPCServer);
    }
    if (outerPort != 0) {
      LOG_INFO("gRPC 对外服务监听成功: {}", metaData_.outerGRPCServer);
    } else {
      LOG_WARNING("gRPC 对外服务监听失败: {}", metaData_.outerGRPCServer);
    }
    selfCheckGrpcConfig("gRPC 服务启动后", metaData_.name, false);
  } else {
    LOG_ERROR("gRPC 服务启动失败，未绑定任何端口");
  }

  LOG_INFO("模块信息:\n名称:\t\t{}\n库名:\t\t{}\n版本:\t\t{}\n内部服务:\t{}\n对外服务:\t{}", metaData_.name, metaData_.libName, metaData_.version.version, metaData_.innerGRPCServer, metaData_.outerGRPCServer);

  serverThread_ = ModuleManager::StartModuleThread(metaData_.name, [this]() {
    if (server_) {
      server_->Wait();
    }
  });
}
void ModuleInterface::shutdownServers() {
  if (server_) {
    server_->Shutdown();
  }
  if (serverThread_.joinable()) {
    serverThread_.join();
  }
  // 当模块卸载后需要将占用的端口释放
  releasePort(metaData_.outerGRPCServer);
}
void ModuleInterface::releasePort(std::string address) {
  auto pos = address.find(':');
  if (pos == std::string::npos) {
    return;
  }
  auto port = address.substr(pos + 1);
  std::lock_guard<std::mutex> lock(portMutex_);
  allocatedPorts_.erase(port);
}
void ModuleInterface::reservePort(std::string address) {
  auto pos = address.find(':');
  if (pos == std::string::npos) {
    return;
  }
  auto port = address.substr(pos + 1);
  std::lock_guard<std::mutex> lock(portMutex_);
  allocatedPorts_.emplace(port);
}
std::string ModuleInterface::getRandomPort() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dist(17001, 17999);

  while (true) {
    auto port = std::to_string(dist(gen));
    std::lock_guard<std::mutex> lock(portMutex_);
    auto [_, inserted] = allocatedPorts_.emplace(port);
    if (inserted) {
      LOG_INFO("已为模块分配对外 gRPC 端口: {}", port);
      return port;
    }
  }
}
}  // namespace ModuleInterface
