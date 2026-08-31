#include "DigitalInput.h"

#include <boost/dll.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "DigitalInputEventProcessor.hpp"
#include "DigitalInputLibInfo.h"
#include "GpioEventReader.hpp"
#include "Logger.h"
#include "ModuleManager.pb.h"
#include "ThreadUtil.hpp"

namespace {
const std::string& GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(DigitalInputLibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(DigitalInputLibInfo.VERSION_MAJOR);
    version->set_minor(DigitalInputLibInfo.VERSION_MINOR);
    version->set_patch(DigitalInputLibInfo.VERSION_PATCH);
    version->set_version(DigitalInputLibInfo.VERSION);

    auto dependency = manifest.add_dependencies();
    dependency->set_module_name("DataCenter");
    dependency->set_version_range("=0.0.1");
    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace DigitalInput {
namespace {

constexpr const char* kDefaultGpioChipPath = "/dev/gpiochip0";

std::string ConfiguredGpioChipPath() {
  const char* configured = std::getenv("MSKDSP_DIGITAL_INPUT_GPIOCHIP");
  if (configured != nullptr && *configured != '\0') {
    return configured;
  }
  return kDefaultGpioChipPath;
}

}  // namespace

DigitalInput::DigitalInput() :
  ModuleInterface(),
  grpcService_(std::make_shared<grpc::Service>()),
  gpioChipPath_(ConfiguredGpioChipPath()) {
  initLibInfo(DigitalInputLibInfo);
}

DigitalInput::~DigitalInput() = default;

void DigitalInput::SetDataCenterStub(
    std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  dataCenter_.SetStub(std::move(stub));
}

void DigitalInput::SetDataCenterServerAddress(std::string address) {
  dataCenter_.SetServerAddress(std::move(address));
}

void DigitalInput::SetGpioChipPath(std::string path) {
  if (!path.empty()) {
    gpioChipPath_ = std::move(path);
  }
}

bool DigitalInput::EnqueuePublished(const PublishedDigitalInput& published) {
  {
    std::lock_guard lock(publishMutex_);
    const auto connId = activeConnectionId_.load(std::memory_order_acquire);
    if (connId == 0) {
      LOG_ERROR("DigitalInput 收到无效连接 ID，丢弃 SOE: tag={}，value={}，ts_ms={}",
                published.tag, published.value, published.timestampMs);
      return false;
    }
    if (pendingPublishes_.size() >= kMaxPendingPublishes) {
      LOG_ERROR("DigitalInput SOE 待发队列已满，丢弃事件: tag={}，value={}，ts_ms={}",
                published.tag, published.value, published.timestampMs);
      return false;
    }
    pendingPublishes_.push_back(PendingPublish{
        .connId = connId,
        .event = published,
    });
  }
  publishCondition_.notify_one();
  return true;
}

bool DigitalInput::RefreshBoardConnection() {
  DataCenterProto::ConnectionInfo connection;
  auto status = dataCenter_.GetOrCreateBoardConnection(&connection);
  if (!status.ok() || connection.conn_id() == 0) {
    LOG_ERROR("DigitalInput 恢复 DataCenter 连接失败: {}", status.error_message());
    return false;
  }
  status = dataCenter_.RegisterBoardTags(connection.conn_id());
  if (!status.ok()) {
    LOG_ERROR("DigitalInput 恢复 DataCenter 标签失败: {}", status.error_message());
    return false;
  }
  {
    std::lock_guard lock(publishMutex_);
    activeConnectionId_.store(connection.conn_id(), std::memory_order_release);
    for (auto& pending : pendingPublishes_) {
      pending.connId = connection.conn_id();
    }
  }
  LOG_INFO("DigitalInput 已恢复 DataCenter 端点: module=DigitalInput, conn=board-di, conn_id={}",
           connection.conn_id());
  return true;
}

void DigitalInput::PublishLoop(std::stop_token stopToken) {
  LOG_INFO("DigitalInput SOE 发布线程启动");
  std::stop_callback callback(stopToken, [this]() { publishCondition_.notify_all(); });
  while (!stopToken.stop_requested()) {
    PendingPublish pending;
    {
      std::unique_lock lock(publishMutex_);
      publishCondition_.wait(lock, [this, &stopToken]() {
        return stopToken.stop_requested() || !pendingPublishes_.empty();
      });
      if (stopToken.stop_requested()) {
        break;
      }
      pending = std::move(pendingPublishes_.front());
      pendingPublishes_.pop_front();
    }

    const auto status = dataCenter_.PublishBool(
        pending.connId, pending.event.tag, pending.event.value,
        pending.event.timestampMs);
    if (status.ok()) {
      LOG_INFO("DigitalInput SOE 已发布: tag={}，value={}，ts_ms={}",
               pending.event.tag, pending.event.value,
               pending.event.timestampMs);
      continue;
    }

    LOG_ERROR("DigitalInput 发布 SOE 失败，将重试: tag={}，原因={}",
              pending.event.tag, status.error_message());
    if (RefreshBoardConnection()) {
      pending.connId = activeConnectionId_.load(std::memory_order_acquire);
    }
    const auto failedTag = pending.event.tag;
    const auto failedValue = pending.event.value;
    const auto failedTimestampMs = pending.event.timestampMs;
    bool requeued = false;
    {
      std::lock_guard lock(publishMutex_);
      if (pendingPublishes_.size() < kMaxPendingPublishes) {
        pendingPublishes_.push_front(std::move(pending));
        requeued = true;
      }
    }
    if (!requeued) {
      LOG_ERROR("DigitalInput SOE 重试入队失败，待发队列已满: tag={}，value={}，ts_ms={}",
                failedTag, failedValue, failedTimestampMs);
    }
    std::unique_lock lock(publishMutex_);
    publishCondition_.wait_for(lock, std::chrono::milliseconds(100),
                               [&stopToken]() {
                                 return stopToken.stop_requested();
                               });
  }
  LOG_INFO("DigitalInput SOE 发布线程停止");
}

void DigitalInput::start(std::stop_token stopToken) {
  LOG_INFO("DigitalInput 模块启动");
  // 模块没有业务 RPC，但仍保留通用 gRPC 监听端点，供 ModuleManager 查询 ready 状态。
  grpcServerBuilder(grpcService_);
  LOG_INFO("DigitalInput 通用 gRPC 监听端点已启动（无业务 RPC）");

  auto publisherThread = ModuleManager::StartModuleThread(
      DigitalInputLibInfo.LIB_NAME,
      [this](std::stop_token token) { PublishLoop(token); });

  while (!stopToken.stop_requested()) {
    DataCenterProto::ConnectionInfo connection;
    auto status = dataCenter_.GetOrCreateBoardConnection(&connection);
    if (!status.ok() || connection.conn_id() == 0) {
      LOG_ERROR("DigitalInput 注册 DataCenter 连接失败: {}", status.error_message());
      WaitBeforeRetry(stopToken);
      continue;
    }
    status = dataCenter_.RegisterBoardTags(connection.conn_id());
    if (!status.ok()) {
      LOG_ERROR("DigitalInput 注册 DataCenter 标签失败: {}", status.error_message());
      WaitBeforeRetry(stopToken);
      continue;
    }
    activeConnectionId_.store(connection.conn_id(), std::memory_order_release);
    LOG_INFO("DigitalInput 已注册 DataCenter 端点: module=DigitalInput, conn=board-di, conn_id={}",
             connection.conn_id());

    GpioEventReader reader(gpioChipPath_);
    std::string error;
    if (!reader.Open(&error)) {
      LOG_ERROR("DigitalInput 打开 GPIO 事件输入失败: {}，将在稍后重试", error);
      WaitBeforeRetry(stopToken);
      continue;
    }
    LOG_INFO("DigitalInput GPIO 事件输入已打开: device={}，ABI={}",
             gpioChipPath_, reader.UsingV2() ? "v2" : "v1");

    DigitalInputEventProcessor processor(
        [this](const PublishedDigitalInput& published) {
          return EnqueuePublished(published);
        });

    while (!stopToken.stop_requested()) {
      GpioEvent event;
      if (!reader.Wait(stopToken, &event)) {
        if (!reader.LastError().empty()) {
          LOG_ERROR("DigitalInput GPIO 事件读取失败: {}，将重新打开设备", reader.LastError());
        }
        break;
      }
      LOG_INFO("DigitalInput 收到 GPIO 边沿: offset={}，physical_high={}，logical_value={}，ts_ms={}",
               event.offset, event.physicalHigh,
               PhysicalLevelToLogical(event.physicalHigh), event.timestampMs);
      if (event.sequenceGap) {
        LOG_ERROR("DigitalInput 检测到 GPIO v2 事件序号断档，可能发生内核 FIFO 溢出: offset={}",
                  event.offset);
      }
      processor.HandleEvent(event);
    }
    reader.Close();
    if (!stopToken.stop_requested()) {
      WaitBeforeRetry(stopToken);
    }
  }

  publisherThread.request_stop();
  publishCondition_.notify_all();
  if (publisherThread.joinable()) {
    publisherThread.join();
  }
  activeConnectionId_.store(0, std::memory_order_release);
  LOG_INFO("DigitalInput 模块停止");
}

void DigitalInput::WaitBeforeRetry(std::stop_token stopToken) {
  std::mutex mutex;
  std::condition_variable_any condition;
  std::stop_callback callback(stopToken, [&condition]() { condition.notify_all(); });
  std::unique_lock lock(mutex);
  condition.wait_for(lock, std::chrono::seconds(1),
                     [&stopToken]() { return stopToken.stop_requested(); });
}

}  // namespace DigitalInput

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new DigitalInput::DigitalInput();
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
