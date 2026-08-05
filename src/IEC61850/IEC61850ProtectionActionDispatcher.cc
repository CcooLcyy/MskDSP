#include "IEC61850ProtectionActionDispatcher.h"

#include <algorithm>
#include <exception>
#include <format>
#include <utility>

#include "Logger.h"

namespace IEC61850 {

namespace {

grpc::Status InvalidArgument(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

grpc::Status InternalError(std::string message) {
  return grpc::Status(grpc::StatusCode::INTERNAL, std::move(message));
}

// 仅用于识别发送线程内的Stop调用，避免该路径尝试join自身。
thread_local ProtectionActionDispatcher* currentDispatcher = nullptr;

}  // namespace

ProtectionActionDispatcher::ProtectionActionDispatcher(
    std::shared_ptr<ProtectionEngine> engine, std::string connName,
    std::size_t capacity, std::size_t maxValueCount, PublishFunction publish,
    ThreadRuntimePolicy runtimePolicy)
    : engine_(std::move(engine)),
      connName_(std::move(connName)),
      publish_(std::move(publish)),
      usableCapacity_(capacity),
      maxValueCount_(maxValueCount),
      runtimePolicy_(std::move(runtimePolicy)) {
  if (usableCapacity_ == 0) {
    usableCapacity_ = 1;
  }
  // 环形队列保留一个空槽来区分满和空，实际分配容量为capacity+1。
  slots_.resize(usableCapacity_ + 1);
  completionSlots_.resize(usableCapacity_ + 1);
  deferredCompletionSlots_.resize(usableCapacity_ + 1);
  for (auto& slot : slots_) {
    slot.values.resize(maxValueCount_);
  }
}

ProtectionActionDispatcher::~ProtectionActionDispatcher() { Stop(); }

grpc::Status ProtectionActionDispatcher::Start() {
  if (engine_ == nullptr || !publish_ || maxValueCount_ == 0) {
    return InvalidArgument("IEC61850保护动作发送器参数不完整");
  }
  std::unique_lock lifecycleLock(lifecycleMutex_);
  if (running_.load(std::memory_order_acquire)) {
    return grpc::Status::OK;
  }
  // 工作线程异常退出后jthread仍然可join；在创建新线程前必须先回收旧线程。
  if (worker_.joinable()) {
    if (worker_.get_id() == std::this_thread::get_id()) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          "IEC61850保护动作发送器不能在发送线程内重新启动");
    }
    stopSource_.request_stop();
    worker_.request_stop();
    waitCondition_.notify_all();
    try {
      worker_.join();
    } catch (const std::exception& exception) {
      return InternalError(std::format("IEC61850回收旧保护动作发送线程失败: {}",
                                       exception.what()));
    } catch (...) {
      return InternalError("IEC61850回收旧保护动作发送线程失败");
    }
  }
  completionAbort_.store(false, std::memory_order_release);
  stopSource_ = std::stop_source{};
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
    return grpc::Status::OK;
  }
  ThreadRuntimeState runtimeState;
  const auto status = StartThreadWithRuntimePolicy(
      &worker_, runtimePolicy_,
      [this](std::stop_token) { Run(stopSource_.get_token()); },
      &runtimeState);
  if (!status.ok()) {
    running_.store(false, std::memory_order_release);
    return status;
  }
  LOG_INFO(
      "IEC61850保护动作发送线程已启动: IED={}, 队列容量={}, 最大成员数={}, 调度={}, 优先级={}, 实际调度读取={}, 亲和性={}, 降级={}",
      connName_, usableCapacity_, maxValueCount_, runtimeState.actualPolicy,
      runtimeState.actualPriority,
      runtimeState.actualStateRead ? "成功" : "失败",
      runtimeState.affinityApplied ? "已设置" : "未设置",
      runtimeState.degraded ? "是" : "否");
  return grpc::Status::OK;
}

void ProtectionActionDispatcher::Stop() noexcept {
  if (currentDispatcher == this) {
    running_.store(false, std::memory_order_release);
    stopSource_.request_stop();
    waitCondition_.notify_all();
    // 当前线程会在发布回调返回后自然退出；不在此处detach或join自身。
    return;
  }
  std::unique_lock lifecycleLock(lifecycleMutex_);
  const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
  if (!wasRunning && !worker_.joinable()) {
    return;
  }
  stopSource_.request_stop();
  worker_.request_stop();
  waitCondition_.notify_all();
  if (worker_.joinable()) {
    try {
      if (worker_.get_id() == std::this_thread::get_id()) {
        // 发送线程不能join自身；保留joinable状态，待外部线程回收，避免
        // detach后对象析构导致发送线程继续访问this。
        LOG_ERROR("IEC61850保护动作发送器不能在发送线程内等待自身结束: IED={}",
                  connName_);
        return;
      } else {
        worker_.join();
      }
    } catch (const std::exception& exception) {
      LOG_ERROR("IEC61850等待保护动作发送线程结束时发生异常: IED={}, 异常信息={}",
                connName_, exception.what());
    } catch (...) {
      LOG_ERROR("IEC61850等待保护动作发送线程结束时发生未知异常: IED={}",
                connName_);
    }
  }
  LOG_INFO("IEC61850保护动作发送线程已停止: IED={}", connName_);
}

bool ProtectionActionDispatcher::TryEnqueue(
    const ProtectionAction& action) noexcept {
  if (!running_.load(std::memory_order_acquire) ||
      action.ruleIndex == std::numeric_limits<std::size_t>::max() ||
      action.outputSubscriptionId == 0 || action.values.empty() ||
      action.values.size() > maxValueCount_) {
    invalidAction_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const auto tail = tail_.load(std::memory_order_relaxed);
  const auto next = (tail + 1) % slots_.size();
  if (next == head_.load(std::memory_order_acquire)) {
    queueFull_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  auto& slot = slots_[tail];
  slot.ruleIndex = action.ruleIndex;
  slot.outputSubscriptionId = action.outputSubscriptionId;
  slot.asserted = action.asserted;
  slot.valueCount = action.values.size();
  std::copy(action.values.begin(), action.values.end(), slot.values.begin());
  tail_.store(next, std::memory_order_release);
  enqueued_.fetch_add(1, std::memory_order_relaxed);
  waitCondition_.notify_one();
  return true;
}

bool ProtectionActionDispatcher::TryDequeue(Slot** slot) noexcept {
  if (slot == nullptr) {
    return false;
  }
  const auto head = head_.load(std::memory_order_relaxed);
  if (head == tail_.load(std::memory_order_acquire)) {
    *slot = nullptr;
    return false;
  }
  *slot = &slots_[head];
  return true;
}

bool ProtectionActionDispatcher::TryEnqueueCompletion(
    const ProtectionActionCompletion& completion) noexcept {
  const auto tail = completionTail_.load(std::memory_order_relaxed);
  const auto next = (tail + 1) % completionSlots_.size();
  if (next == completionHead_.load(std::memory_order_acquire)) {
    return false;
  }
  completionSlots_[tail] = completion;
  completionTail_.store(next, std::memory_order_release);
  waitCondition_.notify_all();
  return true;
}

bool ProtectionActionDispatcher::TryEnqueueDeferredCompletion(
    const ProtectionActionCompletion& completion) noexcept {
  const auto tail = deferredCompletionTail_.load(std::memory_order_relaxed);
  const auto next = (tail + 1) % deferredCompletionSlots_.size();
  if (next ==
      deferredCompletionHead_.load(std::memory_order_acquire)) {
    return false;
  }
  deferredCompletionSlots_[tail] = completion;
  deferredCompletionTail_.store(next, std::memory_order_release);
  deferredCompletions_.fetch_add(1, std::memory_order_relaxed);
  waitCondition_.notify_all();
  return true;
}

bool ProtectionActionDispatcher::TryDequeueCompletion(
    ProtectionActionCompletion* completion) noexcept {
  if (completion == nullptr) {
    return false;
  }
  const auto head = completionHead_.load(std::memory_order_relaxed);
  if (head == completionTail_.load(std::memory_order_acquire)) {
    return false;
  }
  *completion = completionSlots_[head];
  completionHead_.store((head + 1) % completionSlots_.size(),
                        std::memory_order_release);
  waitCondition_.notify_all();
  return true;
}

bool ProtectionActionDispatcher::TryDequeueDeferredCompletion(
    ProtectionActionCompletion* completion) noexcept {
  if (completion == nullptr) {
    return false;
  }
  const auto head =
      deferredCompletionHead_.load(std::memory_order_relaxed);
  if (head == deferredCompletionTail_.load(std::memory_order_acquire)) {
    return false;
  }
  *completion = deferredCompletionSlots_[head];
  deferredCompletionHead_.store((head + 1) % deferredCompletionSlots_.size(),
                                std::memory_order_release);
  waitCondition_.notify_all();
  return true;
}

std::size_t ProtectionActionDispatcher::DrainCompletions(
    std::span<ProtectionActionCompletion> completions) noexcept {
  std::size_t count = 0;
  ProtectionActionCompletion completion;
  while (count < completions.size() && TryDequeueCompletion(&completion)) {
    completions[count++] = completion;
  }
  while (count < completions.size() &&
         TryDequeueDeferredCompletion(&completion)) {
    completions[count++] = completion;
  }
  return count;
}

bool ProtectionActionDispatcher::WaitForCompletionSpace(
    std::stop_token stopToken) noexcept {
  while (true) {
    if (stopToken.stop_requested() ||
        completionAbort_.load(std::memory_order_acquire)) {
      return false;
    }
    const auto tail = completionTail_.load(std::memory_order_relaxed);
    const auto next = (tail + 1) % completionSlots_.size();
    if (next != completionHead_.load(std::memory_order_acquire)) {
      return true;
    }
    std::unique_lock lock(waitMutex_);
    waitCondition_.wait(lock, [this, &stopToken] {
      const auto tail = completionTail_.load(std::memory_order_relaxed);
      const auto next = (tail + 1) % completionSlots_.size();
      return stopToken.stop_requested() ||
             completionAbort_.load(std::memory_order_acquire) ||
             next != completionHead_.load(std::memory_order_acquire);
    });
  }
}

void ProtectionActionDispatcher::QueueCompletionWithoutWaiting(
    Slot& slot, bool success) noexcept {
  ProtectionActionCompletion completion;
  completion.ruleIndex = slot.ruleIndex;
  completion.asserted = slot.asserted;
  completion.success = success;
  if (TryEnqueueCompletion(completion) ||
      TryEnqueueDeferredCompletion(completion)) {
    return;
  }
  completionDropped_.fetch_add(1, std::memory_order_relaxed);
}

void ProtectionActionDispatcher::FailOutstandingActions() noexcept {
  for (;;) {
    Slot* slot = nullptr;
    if (!TryDequeue(&slot)) {
      return;
    }
    if (slot != nullptr) {
      QueueCompletionWithoutWaiting(*slot, false);
    }
    const auto head = head_.load(std::memory_order_relaxed);
    head_.store((head + 1) % slots_.size(), std::memory_order_release);
  }
}

void ProtectionActionDispatcher::QueueCompletion(
    Slot& slot, bool success, std::stop_token stopToken) noexcept {
  ProtectionActionCompletion completion;
  completion.ruleIndex = slot.ruleIndex;
  completion.asserted = slot.asserted;
  completion.success = success;
  // 停止阶段不允许因为完成队列满而阻塞发送线程；当前会话随后会被
  // Manager作废，无法回写的动作完成状态不能继续影响新会话。
  while (!TryEnqueueCompletion(completion)) {
    if (stopToken.stop_requested() ||
        completionAbort_.load(std::memory_order_acquire)) {
      if (!TryEnqueueDeferredCompletion(completion)) {
        completionDropped_.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
    if (!WaitForCompletionSpace(stopToken)) {
      if (!TryEnqueueDeferredCompletion(completion)) {
        completionDropped_.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
  }
}

void ProtectionActionDispatcher::Run(std::stop_token stopToken) noexcept {
  currentDispatcher = this;
  Slot* activeSlot = nullptr;
  try {
    for (;;) {
      Slot* slot = nullptr;
      if (TryDequeue(&slot)) {
        if (slot != nullptr) {
          activeSlot = slot;
          Publish(*slot, stopToken);
          const auto head = head_.load(std::memory_order_relaxed);
          head_.store((head + 1) % slots_.size(), std::memory_order_release);
          activeSlot = nullptr;
        }
        continue;
      }
      if (stopToken.stop_requested() &&
          head_.load(std::memory_order_acquire) ==
              tail_.load(std::memory_order_acquire)) {
        currentDispatcher = nullptr;
        return;
      }
      std::unique_lock lock(waitMutex_);
      waitCondition_.wait(lock, [this, &stopToken] {
        return stopToken.stop_requested() ||
               head_.load(std::memory_order_acquire) !=
                   tail_.load(std::memory_order_acquire);
      });
    }
  } catch (const std::exception& exception) {
    completionAbort_.store(true, std::memory_order_release);
    if (activeSlot != nullptr) {
      QueueCompletionWithoutWaiting(*activeSlot, false);
      const auto head = head_.load(std::memory_order_relaxed);
      head_.store((head + 1) % slots_.size(), std::memory_order_release);
    }
    FailOutstandingActions();
    running_.store(false, std::memory_order_release);
    currentDispatcher = nullptr;
    LOG_ERROR("IEC61850保护动作发送线程发生异常: IED={}, 异常信息={}", connName_,
              exception.what());
  } catch (...) {
    completionAbort_.store(true, std::memory_order_release);
    if (activeSlot != nullptr) {
      QueueCompletionWithoutWaiting(*activeSlot, false);
      const auto head = head_.load(std::memory_order_relaxed);
      head_.store((head + 1) % slots_.size(), std::memory_order_release);
    }
    FailOutstandingActions();
    running_.store(false, std::memory_order_release);
    currentDispatcher = nullptr;
    LOG_ERROR("IEC61850保护动作发送线程发生未知异常: IED={}", connName_);
  }
}

void ProtectionActionDispatcher::Publish(Slot& slot,
                                          std::stop_token stopToken) noexcept {
  grpc::Status status;
  try {
    status = publish_(slot.outputSubscriptionId,
                      std::span<const ProtocolRealtimeValue>(
                          slot.values.data(), slot.valueCount),
                      true);
  } catch (const std::exception& exception) {
    LOG_ERROR("IEC61850保护动作GOOSE发布发生异常: IED={}, 订阅={}, 异常信息={}",
              connName_, slot.outputSubscriptionId, exception.what());
    status = InternalError("IEC61850保护动作GOOSE发布发生异常");
  } catch (...) {
    LOG_ERROR("IEC61850保护动作GOOSE发布发生未知异常: IED={}, 订阅={}",
              connName_, slot.outputSubscriptionId);
    status = InternalError("IEC61850保护动作GOOSE发布发生未知异常");
  }
  QueueCompletion(slot, status.ok(), stopToken);
  if (!status.ok()) {
    publishFailures_.fetch_add(1, std::memory_order_relaxed);
    LOG_WARNING("IEC61850保护动作GOOSE发送失败: IED={}, 订阅={}, 原因={}",
                connName_, slot.outputSubscriptionId, status.error_message());
  }
}

bool ProtectionActionDispatcher::IsRunning() const noexcept {
  return running_.load(std::memory_order_acquire);
}

ProtectionActionDispatcherStatistics
ProtectionActionDispatcher::statistics() const noexcept {
  return {.enqueued = enqueued_.load(std::memory_order_acquire),
          .queueFull = queueFull_.load(std::memory_order_acquire),
          .invalidAction = invalidAction_.load(std::memory_order_acquire),
          .publishFailures = publishFailures_.load(std::memory_order_acquire),
          .deferredCompletions =
              deferredCompletions_.load(std::memory_order_acquire),
          .completionDropped =
              completionDropped_.load(std::memory_order_acquire)};
}

}  // namespace IEC61850
