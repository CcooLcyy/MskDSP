#include "DeviceInfo.h"

#include <boost/dll.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <format>
#include <sstream>
#include <thread>

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include "DeviceInfoLibInfo.h"
#include "Logger.h"
#include "ModuleManager.pb.h"

namespace {
constexpr auto kSampleInterval = std::chrono::seconds(5);
constexpr auto kRpcTimeout = std::chrono::milliseconds(1500);
constexpr const char* kConnectionName = "device-runtime";
constexpr const char* kCpuTag = "cpu.usage_percent";
constexpr const char* kMemoryTag = "memory.usage_percent";

void SetDeadline(grpc::ClientContext* context) {
  context->set_deadline(std::chrono::system_clock::now() + kRpcTimeout);
}

bool ReadFile(const char* path, std::string* out) {
  std::ifstream stream(path);
  if (!stream.is_open()) {
    return false;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  *out = buffer.str();
  return true;
}

const std::string& GetSerializedManifest() {
  static const std::string serialized = [] {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(DeviceInfoLibInfo.LIB_NAME);
    auto* version = manifest.mutable_version();
    version->set_major(DeviceInfoLibInfo.VERSION_MAJOR);
    version->set_minor(DeviceInfoLibInfo.VERSION_MINOR);
    version->set_patch(DeviceInfoLibInfo.VERSION_PATCH);
    version->set_version(DeviceInfoLibInfo.VERSION);
    auto* dependency = manifest.add_dependencies();
    dependency->set_module_name("DataCenter");
    dependency->set_version_range("=0.0.1");
    return manifest.SerializeAsString();
  }();
  return serialized;
}
}  // namespace

namespace DeviceInfo {

DeviceInfo::DeviceInfo() : serverAddress_(DefaultServerAddress()) {
  initLibInfo(DeviceInfoLibInfo);
}

DeviceInfo::~DeviceInfo() = default;

void DeviceInfo::SetDataCenterStub(
    std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  std::lock_guard lock(mutex_);
  stub_ = std::move(stub);
  channel_.reset();
  connectionId_ = 0;
}

void DeviceInfo::SetDataCenterServerAddress(std::string address) {
  std::lock_guard lock(mutex_);
  serverAddress_ = std::move(address);
  stub_.reset();
  channel_.reset();
  connectionId_ = 0;
}

std::shared_ptr<DataCenterProto::DataCenterService::StubInterface>
DeviceInfo::GetStub() {
  std::lock_guard lock(mutex_);
  EnsureStubLocked();
  return stub_;
}

void DeviceInfo::EnsureStubLocked() {
  if (stub_) {
    return;
  }
  channel_ = grpc::CreateChannel(serverAddress_, grpc::InsecureChannelCredentials());
  auto concrete = DataCenterProto::DataCenterService::NewStub(channel_);
  stub_ = std::shared_ptr<DataCenterProto::DataCenterService::StubInterface>(concrete.release());
}

bool DeviceInfo::RegisterDataCenter() {
  DataCenterProto::GetOrCreateConnectionRequest request;
  request.mutable_key()->set_module_name(DeviceInfoLibInfo.LIB_NAME);
  request.mutable_key()->set_conn_name(kConnectionName);
  DataCenterProto::ConnectionInfo response;
  grpc::ClientContext context;
  SetDeadline(&context);
  auto status = GetStub()->GetOrCreateConnection(&context, request, &response);
  if (!status.ok() || response.conn_id() == 0) {
    LOG_ERROR("DeviceInfo 注册 DataCenter 连接失败: {}", status.error_message());
    return false;
  }

  DataCenterProto::UpsertConnTagsRequest tags;
  tags.set_conn_id(response.conn_id());
  tags.set_replace(true);
  tags.add_tags(kCpuTag);
  tags.add_tags(kMemoryTag);
  DataCenterProto::Empty empty;
  grpc::ClientContext tagsContext;
  SetDeadline(&tagsContext);
  status = GetStub()->UpsertConnTags(&tagsContext, tags, &empty);
  if (!status.ok()) {
    LOG_ERROR("DeviceInfo 注册 DataCenter 标签失败: {}", status.error_message());
    return false;
  }
  connectionId_ = response.conn_id();
  LOG_INFO("DeviceInfo DataCenter 连接已注册: conn_id={}",
           connectionId_.load(std::memory_order_relaxed));
  return true;
}

void DeviceInfo::PublishMetrics() {
  std::string cpuContent;
  std::string memoryContent;
  if (!ReadFile("/proc/stat", &cpuContent) || !ReadFile("/proc/meminfo", &memoryContent)) {
    LOG_ERROR("DeviceInfo 读取 /proc 指标文件失败");
    return;
  }
  const auto cpuTimes = ParseCpuTimes(cpuContent);
  const auto cpuUsage = cpuTimes ? cpuTracker_.Update(*cpuTimes) : std::nullopt;
  const auto memoryUsage = ParseMemoryUsage(memoryContent);
  if (!cpuUsage && !memoryUsage) {
    LOG_ERROR("DeviceInfo 本轮未获得有效 CPU 或内存指标");
    return;
  }

  DataCenterProto::BatchPublishRequest request;
  const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  if (cpuUsage) {
    auto* point = request.add_points();
    point->set_conn_id(connectionId_);
    point->set_tag(kCpuTag);
    point->mutable_value()->set_double_value(*cpuUsage);
    point->set_quality(DataCenterProto::QUALITY_GOOD);
    point->set_ts_ms(ts);
  }
  if (memoryUsage) {
    auto* point = request.add_points();
    point->set_conn_id(connectionId_);
    point->set_tag(kMemoryTag);
    point->mutable_value()->set_double_value(*memoryUsage);
    point->set_quality(DataCenterProto::QUALITY_GOOD);
    point->set_ts_ms(ts);
  }
  grpc::ClientContext context;
  SetDeadline(&context);
  DataCenterProto::Empty response;
  const auto status = GetStub()->BatchPublish(&context, request, &response);
  if (!status.ok()) {
    LOG_ERROR("DeviceInfo 发布指标失败: {}", status.error_message());
    connectionId_ = 0;
  }
}

void DeviceInfo::start(std::stop_token stopToken) {
  LOG_INFO("DeviceInfo 模块启动");
  std::mutex waitMutex;
  std::condition_variable_any condition;
  std::stop_callback callback(stopToken, [&condition] { condition.notify_all(); });
  while (!stopToken.stop_requested()) {
    if (connectionId_ == 0 && !RegisterDataCenter()) {
      std::unique_lock lock(waitMutex);
      condition.wait_for(lock, std::chrono::seconds(1), [&stopToken] {
        return stopToken.stop_requested();
      });
      continue;
    }
    PublishMetrics();
    std::unique_lock lock(waitMutex);
    condition.wait_for(lock, kSampleInterval, [&stopToken] {
      return stopToken.stop_requested();
    });
  }
  LOG_INFO("DeviceInfo 模块停止");
}

std::string DeviceInfo::DefaultServerAddress() {
  std::error_code error;
  const auto directory = std::filesystem::path("./socket");
  std::filesystem::create_directories(directory, error);
  const auto absolute = std::filesystem::absolute(directory, error);
  return std::format("unix:{}", ((error ? directory : absolute) / "DataCenter.sock").string());
}

}  // namespace DeviceInfo

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new DeviceInfo::DeviceInfo();
}

extern "C" BOOST_SYMBOL_EXPORT bool GetModuleManifestPb(const uint8_t** data,
                                                          size_t* size) {
  if (data == nullptr || size == nullptr) {
    return false;
  }
  const auto& serialized = GetSerializedManifest();
  *data = reinterpret_cast<const uint8_t*>(serialized.data());
  *size = serialized.size();
  return true;
}
