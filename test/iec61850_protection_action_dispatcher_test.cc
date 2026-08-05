#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include "IEC61850ProtectionActionDispatcher.h"

namespace {

IEC61850::ProtectionAction MakeAction(
    std::vector<IEC61850::ProtocolRealtimeValue>* values,
    std::size_t ruleIndex = 0) {
  values->resize(1);
  values->front().valueType = IEC61850::ProtocolRealtimeValueType::BOOLEAN;
  values->front().value.booleanValue = true;
  IEC61850::ProtectionAction action;
  action.ruleIndex = ruleIndex;
  action.outputSubscriptionId = 1;
  action.asserted = true;
  action.values = *values;
  return action;
}

std::shared_ptr<IEC61850::ProtectionEngine> MakeEngine() {
  return std::make_shared<IEC61850::ProtectionEngine>(
      std::vector<IEC61850::ProtectionRuleConfig>{}, 1);
}

// 验证完成队列已满时停止发送线程仍能有界返回，并保留动作完成结果。
TEST(IEC61850ProtectionActionDispatcherTest,
     StopDoesNotBlockWhenCompletionQueueIsFull) {
  std::atomic<std::size_t> published = 0;
  IEC61850::ProtectionActionDispatcher dispatcher(
      MakeEngine(), "line-1", 1, 1,
      [&published](std::uint32_t,
                   std::span<const IEC61850::ProtocolRealtimeValue>, bool) {
        published.fetch_add(1, std::memory_order_release);
        return grpc::Status::OK;
      });
  ASSERT_TRUE(dispatcher.Start().ok());

  std::vector<IEC61850::ProtocolRealtimeValue> firstValues;
  std::vector<IEC61850::ProtocolRealtimeValue> secondValues;
  ASSERT_TRUE(dispatcher.TryEnqueue(MakeAction(&firstValues, 1)));
  for (int attempt = 0; attempt != 100 &&
                             published.load(std::memory_order_acquire) == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(published.load(std::memory_order_acquire), 1u);
  bool secondEnqueued = false;
  for (int attempt = 0; attempt != 100 && !secondEnqueued; ++attempt) {
    secondEnqueued = dispatcher.TryEnqueue(MakeAction(&secondValues, 2));
    if (!secondEnqueued) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  ASSERT_TRUE(secondEnqueued);

  std::atomic<bool> stopped = false;
  std::thread stopper([&] {
    dispatcher.Stop();
    stopped.store(true, std::memory_order_release);
  });
  for (int attempt = 0; attempt != 200 &&
                             !stopped.load(std::memory_order_acquire);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(stopped.load(std::memory_order_acquire));
  stopper.join();

  std::array<IEC61850::ProtectionActionCompletion, 2> completions;
  ASSERT_EQ(dispatcher.DrainCompletions(completions), 2u);
  EXPECT_TRUE(completions[0].success);
  EXPECT_TRUE(completions[1].success);
}

// 验证发布函数异常会转换为失败完成结果，发送线程不会退出或向外抛异常。
TEST(IEC61850ProtectionActionDispatcherTest,
     PublishExceptionProducesFailureCompletion) {
  IEC61850::ProtectionActionDispatcher dispatcher(
      MakeEngine(), "line-1", 2, 1,
      [](std::uint32_t, std::span<const IEC61850::ProtocolRealtimeValue>,
         bool) -> grpc::Status { throw std::runtime_error("test publish"); });
  ASSERT_TRUE(dispatcher.Start().ok());
  std::vector<IEC61850::ProtocolRealtimeValue> values;
  ASSERT_TRUE(dispatcher.TryEnqueue(MakeAction(&values)));

  std::array<IEC61850::ProtectionActionCompletion, 1> completions;
  std::size_t count = 0;
  for (int attempt = 0; attempt != 100 && count == 0; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    count = dispatcher.DrainCompletions(completions);
  }
  ASSERT_EQ(count, 1u);
  EXPECT_FALSE(completions[0].success);
  EXPECT_TRUE(dispatcher.IsRunning());
  dispatcher.Stop();
}

// 验证发送线程内部调用Stop只请求停止，不detach当前线程；外部再次Stop可以完成join。
TEST(IEC61850ProtectionActionDispatcherTest, SelfStopCanBeJoinedExternally) {
  IEC61850::ProtectionActionDispatcher* current = nullptr;
  std::mutex mutex;
  std::condition_variable condition;
  bool published = false;
  IEC61850::ProtectionActionDispatcher dispatcher(
      MakeEngine(), "line-1", 2, 1,
      [&current, &mutex, &condition, &published](
          std::uint32_t, std::span<const IEC61850::ProtocolRealtimeValue>,
          bool) {
        current->Stop();
        {
          std::lock_guard lock(mutex);
          published = true;
        }
        condition.notify_one();
        return grpc::Status::OK;
      });
  current = &dispatcher;
  ASSERT_TRUE(dispatcher.Start().ok());
  std::vector<IEC61850::ProtocolRealtimeValue> values;
  ASSERT_TRUE(dispatcher.TryEnqueue(MakeAction(&values)));
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(1),
                                   [&published] { return published; }));
  }
  dispatcher.Stop();
  EXPECT_FALSE(dispatcher.IsRunning());
}

// 验证并发Stop只允许一个线程操作jthread，重复启停不会访问已被回收的线程对象。
TEST(IEC61850ProtectionActionDispatcherTest,
     ConcurrentStopAndRestartAreSerialized) {
  IEC61850::ProtectionActionDispatcher dispatcher(
      MakeEngine(), "line-1", 2, 1,
      [](std::uint32_t, std::span<const IEC61850::ProtocolRealtimeValue>,
         bool) { return grpc::Status::OK; });
  ASSERT_TRUE(dispatcher.Start().ok());

  std::vector<std::thread> stoppers;
  for (int index = 0; index != 4; ++index) {
    stoppers.emplace_back([&dispatcher] { dispatcher.Stop(); });
  }
  for (auto& stopper : stoppers) {
    stopper.join();
  }
  EXPECT_FALSE(dispatcher.IsRunning());
  EXPECT_TRUE(dispatcher.Start().ok());
  dispatcher.Stop();
}

// 验证并发Start/Stop不会并发访问jthread生命周期对象。
TEST(IEC61850ProtectionActionDispatcherTest, ConcurrentStartAndStopAreSafe) {
  IEC61850::ProtectionActionDispatcher dispatcher(
      MakeEngine(), "line-1", 2, 1,
      [](std::uint32_t, std::span<const IEC61850::ProtocolRealtimeValue>,
         bool) { return grpc::Status::OK; });
  std::vector<std::thread> workers;
  for (int index = 0; index != 8; ++index) {
    workers.emplace_back([&dispatcher, index] {
      for (int attempt = 0; attempt != 20; ++attempt) {
        if ((index + attempt) % 2 == 0) {
          dispatcher.Start();
        } else {
          dispatcher.Stop();
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  dispatcher.Stop();
}

}  // namespace
