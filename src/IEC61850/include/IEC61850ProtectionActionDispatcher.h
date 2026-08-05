#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850ProtectionEngine.h"
#include "IEC61850ThreadRuntimePolicy.h"

namespace IEC61850 {

// 保护动作发送统计；入队失败不会静默吞掉，调用方可以通过统计诊断队列压力。
struct ProtectionActionDispatcherStatistics {
  std::uint64_t enqueued = 0;
  std::uint64_t queueFull = 0;
  std::uint64_t invalidAction = 0;
  std::uint64_t publishFailures = 0;
  std::uint64_t deferredCompletions = 0;
  std::uint64_t completionDropped = 0;
};

struct ProtectionActionCompletion {
  std::size_t ruleIndex = 0;
  bool asserted = false;
  bool success = false;
};

// 将保护引擎动作从实时消费者线程转移到独立发送线程。
// 队列为单生产者/单消费者，所有槽位及槽位内值在构造时预分配。
class ProtectionActionDispatcher {
public:
  using PublishFunction = std::function<grpc::Status(
      std::uint32_t, std::span<const ProtocolRealtimeValue>, bool)>;

  ProtectionActionDispatcher(std::shared_ptr<ProtectionEngine> engine,
                             std::string connName, std::size_t capacity,
                             std::size_t maxValueCount,
                             PublishFunction publish,
                             ThreadRuntimePolicy runtimePolicy = {});
  ~ProtectionActionDispatcher();

  ProtectionActionDispatcher(const ProtectionActionDispatcher&) = delete;
  ProtectionActionDispatcher& operator=(const ProtectionActionDispatcher&) =
      delete;

  // 启动独立发送线程；重复调用是幂等的。
  grpc::Status Start();
  // 请求发送线程停止并等待已取出动作收敛；完成结果由实时线程回写引擎。
  void Stop() noexcept;

  // 仅供实时生产者调用；不会等待发送线程或执行网络IO。
  bool TryEnqueue(const ProtectionAction& action) noexcept;

  // 仅供实时消费者调用；在同一个ProtectionEngine线程回写发送结果。
  std::size_t DrainCompletions(
      std::span<ProtectionActionCompletion> completions) noexcept;

  bool IsRunning() const noexcept;
  ProtectionActionDispatcherStatistics statistics() const noexcept;

private:
  struct Slot {
    std::size_t ruleIndex = 0;
    std::uint32_t outputSubscriptionId = 0;
    bool asserted = false;
    std::size_t valueCount = 0;
    std::vector<ProtocolRealtimeValue> values;
  };

  bool TryDequeue(Slot** slot) noexcept;
  bool TryEnqueueCompletion(const ProtectionActionCompletion& completion) noexcept;
  bool TryEnqueueDeferredCompletion(
      const ProtectionActionCompletion& completion) noexcept;
  bool TryDequeueCompletion(ProtectionActionCompletion* completion) noexcept;
  bool TryDequeueDeferredCompletion(
      ProtectionActionCompletion* completion) noexcept;
  bool WaitForCompletionSpace(std::stop_token stopToken) noexcept;
  void FailOutstandingActions() noexcept;
  void Run(std::stop_token stopToken) noexcept;
  void QueueCompletion(Slot& slot, bool success,
                       std::stop_token stopToken) noexcept;
  void QueueCompletionWithoutWaiting(Slot& slot, bool success) noexcept;
  void Publish(Slot& slot, std::stop_token stopToken) noexcept;

  std::shared_ptr<ProtectionEngine> engine_;
  std::string connName_;
  PublishFunction publish_;
  std::vector<Slot> slots_;
  std::vector<ProtectionActionCompletion> completionSlots_;
  std::vector<ProtectionActionCompletion> deferredCompletionSlots_;
  std::size_t usableCapacity_ = 0;
  std::size_t maxValueCount_ = 0;
  ThreadRuntimePolicy runtimePolicy_;
  std::atomic<std::size_t> head_ = 0;
  std::atomic<std::size_t> tail_ = 0;
  std::atomic<std::size_t> completionHead_ = 0;
  std::atomic<std::size_t> completionTail_ = 0;
  std::atomic<std::size_t> deferredCompletionHead_ = 0;
  std::atomic<std::size_t> deferredCompletionTail_ = 0;
  // 保护Start/Stop对std::jthread的访问；running_只负责热路径状态判断。
  mutable std::mutex lifecycleMutex_;
  mutable std::mutex waitMutex_;
  std::condition_variable waitCondition_;
  std::stop_source stopSource_;
  std::jthread worker_;
  std::atomic<bool> running_ = false;
  std::atomic<bool> completionAbort_ = false;
  std::atomic<std::uint64_t> enqueued_ = 0;
  std::atomic<std::uint64_t> queueFull_ = 0;
  std::atomic<std::uint64_t> invalidAction_ = 0;
  std::atomic<std::uint64_t> publishFailures_ = 0;
  std::atomic<std::uint64_t> deferredCompletions_ = 0;
  std::atomic<std::uint64_t> completionDropped_ = 0;
};

}  // namespace IEC61850
