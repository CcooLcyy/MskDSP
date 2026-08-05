#include "IEC61850ThreadRuntimePolicy.h"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

#include <gtest/gtest.h>

namespace IEC61850 {
namespace {

std::optional<std::uint32_t> FindAffinityFailureCpu() {
  cpu_set_t original;
  CPU_ZERO(&original);
  if (pthread_getaffinity_np(pthread_self(), sizeof(original), &original) !=
      0) {
    return std::nullopt;
  }
  for (std::uint32_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    cpu_set_t candidate;
    CPU_ZERO(&candidate);
    CPU_SET(static_cast<int>(cpu), &candidate);
    const int error = pthread_setaffinity_np(pthread_self(), sizeof(candidate),
                                             &candidate);
    const int restore = pthread_setaffinity_np(pthread_self(), sizeof(original),
                                               &original);
    if (restore != 0) {
      return std::nullopt;
    }
    if (error != 0) {
      return cpu;
    }
  }
  return std::nullopt;
}

// 验证普通调度策略的默认参数可以在无特权环境下安全应用。
TEST(IEC61850ThreadRuntimePolicyTest, AppliesDefaultPolicyWithoutPrivilege) {
  ThreadRuntimePolicy policy;
  policy.cpuIndices.push_back(static_cast<std::uint32_t>(sched_getcpu()));
  ThreadRuntimeState state;

  const auto status = ApplyThreadRuntimePolicy(policy, &state);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(state.affinityApplied);
  EXPECT_FALSE(state.degraded);
  EXPECT_TRUE(state.actualStateRead);
  EXPECT_EQ(state.actualPolicy, SCHED_OTHER);
  EXPECT_EQ(state.actualPriority, 0);
}

// 验证重复CPU索引不会进入系统调用层，避免配置错误被静默接受。
TEST(IEC61850ThreadRuntimePolicyTest, RejectsDuplicateCpuIndex) {
  ThreadRuntimePolicy policy;
  policy.cpuIndices = {0, 0};

  const auto status = ValidateThreadRuntimePolicy(policy);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("重复"), std::string::npos);
}

// 验证实时优先级超出内核范围时被确定性拒绝。
TEST(IEC61850ThreadRuntimePolicyTest, RejectsOutOfRangeRealtimePriority) {
  ThreadRuntimePolicy policy;
  policy.scheduling = ThreadSchedulingPolicy::FIFO;
  const int maximum = sched_get_priority_max(SCHED_FIFO);
  ASSERT_GE(maximum, 0);
  policy.priority = maximum + 1;

  const auto status = ValidateThreadRuntimePolicy(policy);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("优先级"), std::string::npos);
}

// 验证普通调度策略不能携带实时优先级，避免配置语义被静默忽略。
TEST(IEC61850ThreadRuntimePolicyTest, RejectsPriorityForOtherScheduling) {
  ThreadRuntimePolicy policy;
  policy.priority = 1;

  const auto status = ValidateThreadRuntimePolicy(policy);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("普通调度"), std::string::npos);
}

// 验证非法内部调度枚举不会进入sched_get_priority_*系统调用。
TEST(IEC61850ThreadRuntimePolicyTest, RejectsUnknownSchedulingPolicy) {
  ThreadRuntimePolicy policy;
  policy.scheduling = static_cast<ThreadSchedulingPolicy>(99);

  const auto status = ValidateThreadRuntimePolicy(policy);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("调度策略"), std::string::npos);
}

// 验证CPU索引超出cpu_set_t表达范围时被拒绝。
TEST(IEC61850ThreadRuntimePolicyTest, RejectsCpuIndexOutsideCpuSet) {
  ThreadRuntimePolicy policy;
  policy.cpuIndices = {static_cast<std::uint32_t>(CPU_SETSIZE)};

  const auto status = ValidateThreadRuntimePolicy(policy);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("CPU索引"), std::string::npos);
}

// 验证失败模式枚举只允许降级或严格两种定义值。
TEST(IEC61850ThreadRuntimePolicyTest, RejectsUnknownFailureMode) {
  ThreadRuntimePolicy policy;
  policy.failureMode = static_cast<ThreadRuntimeFailureMode>(99);

  const auto status = ValidateThreadRuntimePolicy(policy);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("失败模式"), std::string::npos);
}

// 验证IED配置可以转换为内部普通调度策略并保留CPU列表。
TEST(IEC61850ThreadRuntimePolicyTest, BuildsPolicyFromIedConfig) {
  IEC61850Proto::IedConfig config;
  config.add_realtime_cpu_indices(0);
  config.add_realtime_cpu_indices(1);
  config.set_realtime_failure_mode(
      IEC61850Proto::THREAD_RUNTIME_FAILURE_MODE_STRICT);

  ThreadRuntimePolicy policy;
  const auto status = BuildThreadRuntimePolicy(config, &policy);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(policy.cpuIndices, (std::vector<std::uint32_t>{0, 1}));
  EXPECT_EQ(policy.scheduling, ThreadSchedulingPolicy::OTHER);
  EXPECT_EQ(policy.failureMode, ThreadRuntimeFailureMode::STRICT);
}

// 验证IED配置中的SCHED_RR和最低实时优先级能够转换为内部策略。
TEST(IEC61850ThreadRuntimePolicyTest, BuildsRoundRobinPolicyFromIedConfig) {
  const int minimum = sched_get_priority_min(SCHED_RR);
  ASSERT_GE(minimum, 0);

  IEC61850Proto::IedConfig config;
  config.set_realtime_scheduling(
      IEC61850Proto::THREAD_SCHEDULING_POLICY_ROUND_ROBIN);
  config.set_realtime_priority(minimum);

  ThreadRuntimePolicy policy;
  const auto status = BuildThreadRuntimePolicy(config, &policy);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(policy.scheduling, ThreadSchedulingPolicy::ROUND_ROBIN);
  EXPECT_EQ(policy.priority, minimum);
}

// 验证配置中的非法调度枚举在转换阶段被拒绝。
TEST(IEC61850ThreadRuntimePolicyTest, RejectsUnknownConfigSchedulingPolicy) {
  IEC61850Proto::IedConfig config;
  config.set_realtime_scheduling(
      static_cast<IEC61850Proto::ThreadSchedulingPolicy>(99));
  ThreadRuntimePolicy policy;

  const auto status = BuildThreadRuntimePolicy(config, &policy);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("枚举"), std::string::npos);
}

// 验证空输出参数不会被线程策略转换函数解引用。
TEST(IEC61850ThreadRuntimePolicyTest, RejectsNullPolicyOutput) {
  IEC61850Proto::IedConfig config;

  const auto status = BuildThreadRuntimePolicy(config, nullptr);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("输出参数"), std::string::npos);
}

// 验证线程策略应用的空状态参数被拒绝，而不是解引用空指针。
TEST(IEC61850ThreadRuntimePolicyTest, RejectsNullRuntimeState) {
  ThreadRuntimePolicy policy;

  const auto status = ApplyThreadRuntimePolicy(policy, nullptr);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("运行状态"), std::string::npos);
}

// 验证统一启动助手会在线程执行任务前应用普通线程策略。
TEST(IEC61850ThreadRuntimePolicyTest, StartsThreadWithRuntimePolicy) {
  ThreadRuntimePolicy policy;
  ThreadRuntimeState state;
  std::atomic<bool> entered = false;
  std::jthread worker;

  const auto status = StartThreadWithRuntimePolicy(
      &worker, policy,
      [&entered](std::stop_token stopToken) {
        entered.store(true, std::memory_order_release);
        while (!stopToken.stop_requested()) {
          std::this_thread::yield();
        }
      },
      &state);

  ASSERT_TRUE(status.ok()) << status.error_message();
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(1);
  while (!entered.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(entered.load(std::memory_order_acquire));
  EXPECT_EQ(state.actualPolicy, SCHED_OTHER);
  EXPECT_TRUE(state.actualStateRead);
  worker.request_stop();
  worker.join();
}

// 验证已有可回收线程的启动目标不会被覆盖，调用方仍可正常停止原线程。
TEST(IEC61850ThreadRuntimePolicyTest, RejectsJoinableWorker) {
  ThreadRuntimePolicy policy;
  std::jthread worker([](std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
      std::this_thread::yield();
    }
  });

  const auto status = StartThreadWithRuntimePolicy(
      &worker, policy, [](std::stop_token) {}, nullptr);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  worker.request_stop();
  worker.join();
}

// 验证实时策略启动参数为空时返回明确的参数错误。
TEST(IEC61850ThreadRuntimePolicyTest, RejectsIncompleteThreadStartArguments) {
  ThreadRuntimePolicy policy;
  std::jthread worker;

  const auto nullWorker = StartThreadWithRuntimePolicy(
      nullptr, policy, [](std::stop_token) {}, nullptr);
  EXPECT_EQ(nullWorker.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  const auto emptyEntry = StartThreadWithRuntimePolicy(
      &worker, policy, ThreadRuntimeEntry{}, nullptr);
  EXPECT_EQ(emptyEntry.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证DEGRADE模式在CPU亲和性设置失败时仍执行任务并记录降级状态。
TEST(IEC61850ThreadRuntimePolicyTest, DegradeContinuesWhenAffinityFails) {
  const auto failureCpu = FindAffinityFailureCpu();
  if (!failureCpu.has_value()) {
    GTEST_SKIP() << "当前进程可使用全部CPU，无法构造亲和性失败场景";
  }

  ThreadRuntimePolicy policy;
  policy.cpuIndices = {*failureCpu};
  policy.failureMode = ThreadRuntimeFailureMode::DEGRADE;
  ThreadRuntimeState state;
  std::atomic<bool> entered = false;
  std::jthread worker;

  const auto status = StartThreadWithRuntimePolicy(
      &worker, policy,
      [&entered](std::stop_token stopToken) {
        entered.store(true, std::memory_order_release);
        while (!stopToken.stop_requested()) {
          std::this_thread::yield();
        }
      },
      &state);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(state.degraded);
  EXPECT_NE(state.errorNumber, 0);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(1);
  while (!entered.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(entered.load(std::memory_order_acquire));
  worker.request_stop();
  worker.join();
}

// 验证STRICT模式在CPU亲和性设置失败时不执行任务并返回错误。
TEST(IEC61850ThreadRuntimePolicyTest, StrictStopsWhenAffinityFails) {
  const auto failureCpu = FindAffinityFailureCpu();
  if (!failureCpu.has_value()) {
    GTEST_SKIP() << "当前进程可使用全部CPU，无法构造亲和性失败场景";
  }

  ThreadRuntimePolicy policy;
  policy.cpuIndices = {*failureCpu};
  policy.failureMode = ThreadRuntimeFailureMode::STRICT;
  ThreadRuntimeState state;
  std::atomic<bool> entered = false;
  std::jthread worker;

  const auto status = StartThreadWithRuntimePolicy(
      &worker, policy,
      [&entered](std::stop_token) {
        entered.store(true, std::memory_order_release);
      },
      &state);

  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(status.error_code() == grpc::StatusCode::UNAVAILABLE ||
              status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
  EXPECT_TRUE(state.degraded);
  EXPECT_FALSE(entered.load(std::memory_order_acquire));
  EXPECT_FALSE(worker.joinable());
}

// 验证FIFO和RR配置都能通过策略范围校验，实际权限交由线程启动阶段判定。
TEST(IEC61850ThreadRuntimePolicyTest, ValidatesRealtimeSchedulingPolicies) {
  for (const auto scheduling : {ThreadSchedulingPolicy::FIFO,
                                ThreadSchedulingPolicy::ROUND_ROBIN}) {
    ThreadRuntimePolicy policy;
    policy.scheduling = scheduling;
    policy.priority = sched_get_priority_min(
        scheduling == ThreadSchedulingPolicy::FIFO ? SCHED_FIFO : SCHED_RR);
    ASSERT_GE(policy.priority, 0);

    const auto status = ValidateThreadRuntimePolicy(policy);

    EXPECT_TRUE(status.ok()) << status.error_message();
  }
}

// 验证严格实时策略在权限不足时返回可诊断错误；具备权限的环境也必须真正生效。
TEST(IEC61850ThreadRuntimePolicyTest, ReportsStrictRealtimePermissionOutcome) {
  ThreadRuntimePolicy policy;
  policy.scheduling = ThreadSchedulingPolicy::FIFO;
  policy.priority = sched_get_priority_min(SCHED_FIFO);
  policy.failureMode = ThreadRuntimeFailureMode::STRICT;
  ASSERT_GE(policy.priority, 0);

  ThreadRuntimeState state;
  const auto status = ApplyThreadRuntimePolicy(policy, &state);

  if (!status.ok()) {
    EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_NE(status.error_message().find("调度"), std::string::npos);
    EXPECT_TRUE(state.degraded);
  } else {
    EXPECT_TRUE(state.schedulingApplied);
    EXPECT_TRUE(state.actualStateRead);
    EXPECT_EQ(state.actualPolicy, SCHED_FIFO);
    EXPECT_EQ(state.actualPriority, policy.priority);
    sched_param restore{};
    EXPECT_EQ(pthread_setschedparam(pthread_self(), SCHED_OTHER, &restore), 0);
  }
}

// 验证SCHED_RR在无权限环境下返回权限错误，有权限环境下真正应用并恢复普通调度。
TEST(IEC61850ThreadRuntimePolicyTest, ReportsStrictRoundRobinPermissionOutcome) {
  ThreadRuntimePolicy policy;
  policy.scheduling = ThreadSchedulingPolicy::ROUND_ROBIN;
  policy.priority = sched_get_priority_min(SCHED_RR);
  policy.failureMode = ThreadRuntimeFailureMode::STRICT;
  ASSERT_GE(policy.priority, 0);

  ThreadRuntimeState state;
  const auto status = ApplyThreadRuntimePolicy(policy, &state);

  if (!status.ok()) {
    EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_NE(status.error_message().find("调度"), std::string::npos);
    EXPECT_TRUE(state.degraded);
  } else {
    EXPECT_TRUE(state.schedulingApplied);
    EXPECT_TRUE(state.actualStateRead);
    EXPECT_EQ(state.actualPolicy, SCHED_RR);
    EXPECT_EQ(state.actualPriority, policy.priority);
    sched_param restore{};
    EXPECT_EQ(pthread_setschedparam(pthread_self(), SCHED_OTHER, &restore), 0);
  }
}

}  // namespace
}  // namespace IEC61850
