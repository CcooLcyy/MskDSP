#include "BoardIO.h"

#include <boost/dll.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "BoardIOLibInfo.h"
#include "BoardInputEventProcessor.hpp"
#include "GpioEventReader.hpp"
#include "Logger.h"
#include "ModuleManager.pb.h"
#include "ThreadUtil.hpp"

namespace {
const std::string& GetSerializedManifest() {
  static const std::string kSerialized = []() {
    ModuleManagerProto::ModuleManifest manifest;
    manifest.set_module_name(BoardIOLibInfo.LIB_NAME);
    auto version = manifest.mutable_version();
    version->set_major(BoardIOLibInfo.VERSION_MAJOR);
    version->set_minor(BoardIOLibInfo.VERSION_MINOR);
    version->set_patch(BoardIOLibInfo.VERSION_PATCH);
    version->set_version(BoardIOLibInfo.VERSION);

    auto dependency = manifest.add_dependencies();
    dependency->set_module_name("DataCenter");
    dependency->set_version_range("=0.0.1");
    return manifest.SerializeAsString();
  }();
  return kSerialized;
}
}  // namespace

namespace BoardIO {
namespace {

// 目标板 0~351 号 GPIO 所在的 3604000.pinctrl 对应 gpiochip1；
// 可通过环境变量覆盖，便于适配其他板卡的设备节点编号。
constexpr const char* kDefaultGpioChipPath = "/dev/gpiochip1";

std::string ConfiguredGpioChipPath() {
  const char* configured = std::getenv("MSKDSP_BOARD_IO_GPIOCHIP");
  if (configured != nullptr && *configured != '\0') {
    return configured;
  }
  return kDefaultGpioChipPath;
}

}  // namespace

BoardIO::BoardIO() :
  ModuleInterface(),
  grpcService_(std::make_shared<grpc::Service>()),
  gpioChipPath_(ConfiguredGpioChipPath()),
  commandService_(std::make_shared<BoardIOCommandService>(
      &outputManager_, &activeOutputConnectionId_)) {
  initLibInfo(BoardIOLibInfo);
}

BoardIO::~BoardIO() = default;

void BoardIO::SetDataCenterStub(
    std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  dataCenter_.SetStub(std::move(stub));
}

void BoardIO::SetDataCenterServerAddress(std::string address) {
  dataCenter_.SetServerAddress(std::move(address));
}

void BoardIO::SetGpioChipPath(std::string path) {
  if (!path.empty()) {
    gpioChipPath_ = std::move(path);
  }
}

bool BoardIO::EnqueuePublished(const PublishedBoardInput& published) {
  {
    std::lock_guard lock(publishMutex_);
    const auto connId = activeInputConnectionId_.load(std::memory_order_acquire);
    if (connId == 0) {
      LOG_ERROR("BoardIO 收到无效遥信连接 ID，丢弃 SOE: tag={}，value={}，ts_ms={}",
                published.tag, published.value, published.timestampMs);
      return false;
    }
    if (pendingPublishes_.size() >= kMaxPendingPublishes) {
      LOG_ERROR("BoardIO SOE 待发队列已满，丢弃事件: tag={}，value={}，ts_ms={}",
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

bool BoardIO::RegisterDataCenterEndpoints() {
  std::lock_guard registrationLock(registrationMutex_);
  const auto previousInputConnectionId =
      activeInputConnectionId_.load(std::memory_order_acquire);
  const auto previousOutputConnectionId =
      activeOutputConnectionId_.load(std::memory_order_acquire);

  DataCenterProto::ConnectionInfo inputConnection;
  auto status = dataCenter_.GetOrMigrateInputConnection(&inputConnection);
  if (!status.ok() || inputConnection.conn_id() == 0) {
    LOG_ERROR("BoardIO 注册 DataCenter 遥信连接失败: {}",
              status.error_message());
    return false;
  }
  status = dataCenter_.EnsureInputTags(inputConnection.conn_id());
  if (!status.ok()) {
    LOG_ERROR("BoardIO 注册 DataCenter 遥信标签失败: {}",
              status.error_message());
    return false;
  }
  DataCenterProto::ConnectionInfo outputConnection;
  status = dataCenter_.GetOrCreateOutputConnection(&outputConnection);
  if (!status.ok() || outputConnection.conn_id() == 0) {
    LOG_ERROR("BoardIO 注册 DataCenter 遥控连接失败: {}",
              status.error_message());
    return false;
  }
  if (previousOutputConnectionId != 0 &&
      previousOutputConnectionId != outputConnection.conn_id()) {
    activeOutputConnectionId_.store(0, std::memory_order_release);
    LOG_WARNING("BoardIO 检测到 DataCenter 遥控连接 ID 变化，完成标签校验前暂停遥控: old_conn_id={}，new_conn_id={}",
                previousOutputConnectionId, outputConnection.conn_id());
  }
  status = dataCenter_.EnsureOutputTags(outputConnection.conn_id());
  if (!status.ok()) {
    LOG_ERROR("BoardIO 注册 DataCenter 遥控标签失败: {}",
              status.error_message());
    return false;
  }
  {
    std::lock_guard lock(publishMutex_);
    activeInputConnectionId_.store(inputConnection.conn_id(),
                                   std::memory_order_release);
    for (auto& pending : pendingPublishes_) {
      pending.connId = inputConnection.conn_id();
    }
  }
  activeOutputConnectionId_.store(outputConnection.conn_id(),
                                  std::memory_order_release);
  if (previousInputConnectionId != inputConnection.conn_id()) {
    LOG_INFO("BoardIO 已更新 DataCenter 遥信端点: module=BoardIO, conn=board-di, conn_id={}",
             inputConnection.conn_id());
  }
  if (previousOutputConnectionId != outputConnection.conn_id()) {
    LOG_INFO("BoardIO 已更新 DataCenter 遥控端点: module=BoardIO, conn=board-do, conn_id={}",
             outputConnection.conn_id());
  }
  return true;
}

bool BoardIO::RefreshDataCenterEndpoints() {
  const bool refreshed = RegisterDataCenterEndpoints();
  if (!refreshed) {
    LOG_ERROR("BoardIO 恢复 DataCenter 遥信/遥控端点失败");
  }
  return refreshed;
}

void BoardIO::DataCenterRegistrationLoop(std::stop_token stopToken) {
  LOG_INFO("BoardIO DataCenter 端点自愈线程启动");
  while (!stopToken.stop_requested()) {
    RefreshDataCenterEndpoints();
    for (int elapsedSeconds = 0;
         elapsedSeconds < 5 && !stopToken.stop_requested();
         ++elapsedSeconds) {
      WaitBeforeRetry(stopToken);
    }
  }
  LOG_INFO("BoardIO DataCenter 端点自愈线程停止");
}

void BoardIO::OutputLoop(std::stop_token stopToken) {
  LOG_INFO("BoardIO 板载输出管理线程启动");
  while (!stopToken.stop_requested() && !outputManager_.Ready()) {
    std::string error;
    if (outputManager_.Start(gpioChipPath_, &error)) {
      break;
    }
    LOG_ERROR("BoardIO 启动板载输出功能失败: {}，将在稍后重试", error);
    WaitBeforeRetry(stopToken);
  }
  while (!stopToken.stop_requested()) {
    WaitBeforeRetry(stopToken);
  }

  std::string error;
  if (!outputManager_.Stop(&error)) {
    LOG_ERROR("BoardIO 停止板载输出功能失败: {}", error);
  }
  LOG_INFO("BoardIO 板载输出管理线程停止");
}

void BoardIO::PublishLoop(std::stop_token stopToken) {
  LOG_INFO("BoardIO SOE 发布线程启动");
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
      LOG_INFO("BoardIO SOE 已发布: tag={}，value={}，ts_ms={}",
               pending.event.tag, pending.event.value,
               pending.event.timestampMs);
      continue;
    }

    LOG_ERROR("BoardIO 发布 SOE 失败，将重试: tag={}，原因={}",
              pending.event.tag, status.error_message());
    if (RefreshDataCenterEndpoints()) {
      pending.connId =
          activeInputConnectionId_.load(std::memory_order_acquire);
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
      LOG_ERROR("BoardIO SOE 重试入队失败，待发队列已满: tag={}，value={}，ts_ms={}",
                failedTag, failedValue, failedTimestampMs);
    }
    std::unique_lock lock(publishMutex_);
    publishCondition_.wait_for(lock, std::chrono::milliseconds(100),
                               [&stopToken]() {
                                 return stopToken.stop_requested();
                               });
  }
  LOG_INFO("BoardIO SOE 发布线程停止");
}

void BoardIO::start(std::stop_token stopToken) {
  LOG_INFO("BoardIO 模块启动");
  grpcServerBuilder(std::vector<std::shared_ptr<grpc::Service>>{
      grpcService_, commandService_});
  LOG_INFO("BoardIO 通用 gRPC 与同步遥控服务监听端点已启动");

  auto publisherThread = ModuleManager::StartModuleThread(
      BoardIOLibInfo.LIB_NAME,
      [this](std::stop_token token) { PublishLoop(token); });
  auto outputThread = ModuleManager::StartModuleThread(
      BoardIOLibInfo.LIB_NAME,
      [this](std::stop_token token) { OutputLoop(token); });
  auto registrationThread = ModuleManager::StartModuleThread(
      BoardIOLibInfo.LIB_NAME,
      [this](std::stop_token token) { DataCenterRegistrationLoop(token); });

  while (!stopToken.stop_requested()) {
    if (activeInputConnectionId_.load(std::memory_order_acquire) == 0) {
      WaitBeforeRetry(stopToken);
      continue;
    }

    GpioEventReader reader(gpioChipPath_);
    std::string error;
    if (!reader.Open(&error)) {
      LOG_ERROR("BoardIO 打开 GPIO 事件输入失败: {}，将在稍后重试", error);
      WaitBeforeRetry(stopToken);
      continue;
    }
    LOG_INFO("BoardIO GPIO 事件输入已打开: device={}，ABI={}",
             gpioChipPath_, reader.UsingV2() ? "v2" : "v1");

    BoardInputEventProcessor processor(
        [this](const PublishedBoardInput& published) {
          return EnqueuePublished(published);
        });

    while (!stopToken.stop_requested()) {
      GpioEvent event;
      if (!reader.Wait(stopToken, &event)) {
        if (!reader.LastError().empty()) {
          LOG_ERROR("BoardIO GPIO 事件读取失败: {}，将重新打开设备", reader.LastError());
        }
        break;
      }
      LOG_INFO("BoardIO 收到 GPIO 边沿: offset={}，physical_high={}，logical_value={}，ts_ms={}",
               event.offset, event.physicalHigh,
               PhysicalLevelToLogical(event.physicalHigh), event.timestampMs);
      if (event.sequenceGap) {
        LOG_ERROR("BoardIO 检测到 GPIO v2 事件序号断档，可能发生内核 FIFO 溢出: offset={}",
                  event.offset);
      }
      processor.HandleEvent(event);
    }
    reader.Close();
    if (!stopToken.stop_requested()) {
      WaitBeforeRetry(stopToken);
    }
  }

  registrationThread.request_stop();
  if (registrationThread.joinable()) {
    registrationThread.join();
  }
  activeOutputConnectionId_.store(0, std::memory_order_release);
  outputThread.request_stop();
  publisherThread.request_stop();
  publishCondition_.notify_all();
  if (publisherThread.joinable()) {
    publisherThread.join();
  }
  if (outputThread.joinable()) {
    outputThread.join();
  }
  activeInputConnectionId_.store(0, std::memory_order_release);
  LOG_INFO("BoardIO 模块停止");
}

void BoardIO::WaitBeforeRetry(std::stop_token stopToken) {
  std::mutex mutex;
  std::condition_variable_any condition;
  std::stop_callback callback(stopToken, [&condition]() { condition.notify_all(); });
  std::unique_lock lock(mutex);
  condition.wait_for(lock, std::chrono::seconds(1),
                     [&stopToken]() { return stopToken.stop_requested(); });
}

}  // namespace BoardIO

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new BoardIO::BoardIO();
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
