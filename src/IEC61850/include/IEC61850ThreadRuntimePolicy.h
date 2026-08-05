#pragma once

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850.pb.h"

namespace IEC61850 {

// 线程调度策略；普通调度不改变当前进程的默认行为。
enum class ThreadSchedulingPolicy : std::uint8_t {
  OTHER = 0,
  FIFO = 1,
  ROUND_ROBIN = 2,
};

// 实时属性设置失败时是返回错误，还是保留普通调度继续运行。
enum class ThreadRuntimeFailureMode : std::uint8_t {
  DEGRADE = 0,
  STRICT = 1,
};

struct ThreadRuntimePolicy {
  // 空集合表示不修改CPU亲和性；索引必须属于系统可表达的CPU集合。
  std::vector<std::uint32_t> cpuIndices;
  ThreadSchedulingPolicy scheduling = ThreadSchedulingPolicy::OTHER;
  // OTHER策略只能使用0；FIFO/RR必须在系统允许的优先级范围内。
  int priority = 0;
  ThreadRuntimeFailureMode failureMode = ThreadRuntimeFailureMode::DEGRADE;
};

struct ThreadRuntimeState {
  bool affinityApplied = false;
  bool schedulingApplied = false;
  bool degraded = false;
  // 实际调度参数是否成功从pthread读取；失败时不能把默认值当作现场结果。
  bool actualStateRead = false;
  int errorNumber = 0;
  int actualPolicy = 0;
  int actualPriority = 0;
  std::string message;
};

grpc::Status ValidateThreadRuntimePolicy(const ThreadRuntimePolicy& policy);

// 将IED配置中的线程策略转换为内部策略并完成字段校验。
grpc::Status BuildThreadRuntimePolicy(
    const IEC61850Proto::IedConfig& config, ThreadRuntimePolicy* policy);

// 在线程入口同步应用策略；严格模式失败时线程不会执行任务函数。
using ThreadRuntimeEntry = std::function<void(std::stop_token)>;
grpc::Status StartThreadWithRuntimePolicy(
    std::jthread* worker, const ThreadRuntimePolicy& policy,
    ThreadRuntimeEntry entry, ThreadRuntimeState* state = nullptr);

// 将策略应用到当前线程。DEGRADE模式下权限不足会返回OK并设置degraded，
// STRICT模式下会返回明确的PERMISSION_DENIED或UNAVAILABLE状态。
grpc::Status ApplyThreadRuntimePolicy(const ThreadRuntimePolicy& policy,
                                      ThreadRuntimeState* state);

}  // namespace IEC61850
