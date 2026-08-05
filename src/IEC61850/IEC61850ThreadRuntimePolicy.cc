#include "IEC61850ThreadRuntimePolicy.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <future>
#include <memory>
#include <pthread.h>
#include <ranges>
#include <sched.h>
#include <string>
#include <thread>
#include <utility>

#include "Logger.h"

namespace IEC61850 {
namespace {

grpc::Status Invalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

grpc::Status RuntimeError(int errorNumber, std::string_view operation) {
  const auto code = errorNumber == EPERM || errorNumber == EACCES
                        ? grpc::StatusCode::PERMISSION_DENIED
                        : grpc::StatusCode::UNAVAILABLE;
  return grpc::Status(code, std::format("{}失败: {}", operation,
                                         std::strerror(errorNumber)));
}

int ToNativePolicy(ThreadSchedulingPolicy policy) noexcept {
  switch (policy) {
    case ThreadSchedulingPolicy::OTHER:
      return SCHED_OTHER;
    case ThreadSchedulingPolicy::FIFO:
      return SCHED_FIFO;
    case ThreadSchedulingPolicy::ROUND_ROBIN:
      return SCHED_RR;
  }
  return -1;
}

}  // namespace

grpc::Status ValidateThreadRuntimePolicy(const ThreadRuntimePolicy& policy) {
  if (ToNativePolicy(policy.scheduling) < 0) {
    return Invalid("IEC61850线程调度策略无效");
  }
  if (policy.failureMode != ThreadRuntimeFailureMode::DEGRADE &&
      policy.failureMode != ThreadRuntimeFailureMode::STRICT) {
    return Invalid("IEC61850线程运行策略失败模式无效");
  }
  if (policy.cpuIndices.size() > CPU_SETSIZE) {
    return Invalid("IEC61850线程CPU集合数量超过系统上限");
  }
  std::vector<std::uint32_t> sorted = policy.cpuIndices;
  std::ranges::sort(sorted);
  if (std::ranges::adjacent_find(sorted) != sorted.end()) {
    return Invalid("IEC61850线程CPU集合包含重复索引");
  }
  if (std::ranges::any_of(sorted, [](std::uint32_t cpu) {
        return cpu >= CPU_SETSIZE;
      })) {
    return Invalid("IEC61850线程CPU索引超出系统表达范围");
  }
  if (policy.scheduling == ThreadSchedulingPolicy::OTHER) {
    if (policy.priority != 0) {
      return Invalid("IEC61850普通调度策略的优先级必须为0");
    }
    return grpc::Status::OK;
  }
  const auto nativePolicy = ToNativePolicy(policy.scheduling);
  const int minimum = sched_get_priority_min(nativePolicy);
  const int maximum = sched_get_priority_max(nativePolicy);
  if (minimum < 0 || maximum < 0 || policy.priority < minimum ||
      policy.priority > maximum) {
    return Invalid(std::format("IEC61850线程实时优先级超出范围: {}-{}",
                               minimum, maximum));
  }
  return grpc::Status::OK;
}

grpc::Status BuildThreadRuntimePolicy(
    const IEC61850Proto::IedConfig& config, ThreadRuntimePolicy* policy) {
  if (policy == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850线程运行策略输出参数为空");
  }
  ThreadRuntimePolicy result;
  result.cpuIndices.assign(config.realtime_cpu_indices().begin(),
                           config.realtime_cpu_indices().end());
  switch (config.realtime_scheduling()) {
    case IEC61850Proto::THREAD_SCHEDULING_POLICY_UNSPECIFIED:
      result.scheduling = ThreadSchedulingPolicy::OTHER;
      break;
    case IEC61850Proto::THREAD_SCHEDULING_POLICY_FIFO:
      result.scheduling = ThreadSchedulingPolicy::FIFO;
      break;
    case IEC61850Proto::THREAD_SCHEDULING_POLICY_ROUND_ROBIN:
      result.scheduling = ThreadSchedulingPolicy::ROUND_ROBIN;
      break;
    default:
      return Invalid("IEC61850 IED线程调度策略枚举值无效");
  }
  result.priority = config.realtime_priority();
  switch (config.realtime_failure_mode()) {
    case IEC61850Proto::THREAD_RUNTIME_FAILURE_MODE_UNSPECIFIED:
    case IEC61850Proto::THREAD_RUNTIME_FAILURE_MODE_DEGRADE:
      result.failureMode = ThreadRuntimeFailureMode::DEGRADE;
      break;
    case IEC61850Proto::THREAD_RUNTIME_FAILURE_MODE_STRICT:
      result.failureMode = ThreadRuntimeFailureMode::STRICT;
      break;
    default:
      return Invalid("IEC61850 IED线程策略失败模式枚举值无效");
  }
  const auto status = ValidateThreadRuntimePolicy(result);
  if (!status.ok()) {
    return status;
  }
  *policy = std::move(result);
  return grpc::Status::OK;
}

grpc::Status ApplyThreadRuntimePolicy(const ThreadRuntimePolicy& policy,
                                      ThreadRuntimeState* state) {
  if (state == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850线程运行状态参数为空");
  }
  *state = {};
  const auto validation = ValidateThreadRuntimePolicy(policy);
  if (!validation.ok()) {
    state->message = validation.error_message();
    return validation;
  }

  const auto handleFailure = [&](int errorNumber,
                                 std::string_view operation) -> grpc::Status {
    state->errorNumber = errorNumber;
    state->degraded = true;
    state->message = std::format("{}失败: {}", operation,
                                 std::strerror(errorNumber));
    if (policy.failureMode == ThreadRuntimeFailureMode::STRICT) {
      return RuntimeError(errorNumber, operation);
    }
    LOG_WARNING("IEC61850线程运行策略降级: {}", state->message);
    return grpc::Status::OK;
  };

  if (!policy.cpuIndices.empty()) {
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    for (const auto cpu : policy.cpuIndices) {
      CPU_SET(cpu, &cpuSet);
    }
    const int errorNumber = pthread_setaffinity_np(
        pthread_self(), sizeof(cpuSet), &cpuSet);
    if (errorNumber != 0) {
      const auto status = handleFailure(errorNumber, "设置IEC61850线程CPU亲和性");
      if (!status.ok()) {
        return status;
      }
    } else {
      state->affinityApplied = true;
    }
  }

  const auto nativePolicy = ToNativePolicy(policy.scheduling);
  if (nativePolicy != SCHED_OTHER) {
    sched_param parameter{};
    parameter.sched_priority = policy.priority;
    const int errorNumber =
        pthread_setschedparam(pthread_self(), nativePolicy, &parameter);
    if (errorNumber != 0) {
      const auto status = handleFailure(errorNumber, "设置IEC61850线程实时调度");
      if (!status.ok()) {
        return status;
      }
    } else {
      state->schedulingApplied = true;
    }
  }

  int actualPolicy = SCHED_OTHER;
  sched_param actualParameter{};
  const int inspectError =
      pthread_getschedparam(pthread_self(), &actualPolicy, &actualParameter);
  if (inspectError == 0) {
    state->actualPolicy = actualPolicy;
    state->actualPriority = actualParameter.sched_priority;
    state->actualStateRead = true;
  } else {
    if (state->errorNumber == 0) {
      state->errorNumber = inspectError;
    }
    state->degraded = true;
    state->message = std::format("读取IEC61850线程实时调度失败: {}",
                                 std::strerror(inspectError));
    if (policy.failureMode == ThreadRuntimeFailureMode::STRICT) {
      return RuntimeError(inspectError, "读取IEC61850线程实时调度");
    }
    LOG_WARNING("IEC61850线程运行策略降级: {}", state->message);
  }

  if (!state->degraded && state->actualStateRead) {
    state->message = "IEC61850线程运行策略已应用";
    LOG_INFO("{}: policy={}, priority={}, affinity={}", state->message,
             state->actualPolicy, state->actualPriority,
             state->affinityApplied ? "已设置" : "未设置");
  }
  return grpc::Status::OK;
}

grpc::Status StartThreadWithRuntimePolicy(
    std::jthread* worker, const ThreadRuntimePolicy& policy,
    ThreadRuntimeEntry entry, ThreadRuntimeState* state) {
  if (worker == nullptr || !entry) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850线程启动参数不完整");
  }
  if (worker->joinable()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850线程启动目标仍有可回收线程");
  }
  const auto validation = ValidateThreadRuntimePolicy(policy);
  if (!validation.ok()) {
    if (state != nullptr) {
      *state = {};
      state->message = validation.error_message();
    }
    return validation;
  }

  auto readyPromise = std::make_shared<std::promise<grpc::Status>>();
  auto readyFuture = readyPromise->get_future();
  auto appliedState = std::make_shared<ThreadRuntimeState>();
  try {
    *worker = std::jthread(
        [policy, entry = std::move(entry), readyPromise, appliedState](
            std::stop_token stopToken) mutable {
          const auto status =
              ApplyThreadRuntimePolicy(policy, appliedState.get());
          readyPromise->set_value(status);
          if (!status.ok()) {
            LOG_ERROR("IEC61850线程运行策略严格应用失败: {}",
                      status.error_message());
            return;
          }
          try {
            entry(stopToken);
          } catch (const std::exception& exception) {
            LOG_ERROR("IEC61850策略线程发生异常: {}", exception.what());
          } catch (...) {
            LOG_ERROR("IEC61850策略线程发生未知异常");
          }
        });
  } catch (const std::exception& exception) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        std::format("IEC61850策略线程启动失败: {}",
                                    exception.what()));
  } catch (...) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "IEC61850策略线程启动失败");
  }

  const auto status = readyFuture.get();
  if (state != nullptr) {
    *state = *appliedState;
  }
  if (!status.ok()) {
    worker->request_stop();
    if (worker->joinable()) {
      worker->join();
    }
    return status;
  }
  return grpc::Status::OK;
}

}  // namespace IEC61850
