#include "IEC61850CommandExecutorService.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <thread>

#include "IEC61850Manager.h"
#include "Logger.h"

namespace IEC61850 {

void IEC61850CommandExecutorServiceImpl::SetManager(
    Manager* manager) noexcept {
  manager_ = manager;
}

grpc::Status IEC61850CommandExecutorServiceImpl::ExecuteCommand(
    grpc::ServerContext* context,
    const DataCenterProto::ExecuteCommandRequest* request,
    DataCenterProto::ExecuteCommandResponse* response) {
  if (context != nullptr && context->IsCancelled()) {
    LOG_WARNING("IEC61850同步命令在进入控制流程前已取消");
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850同步命令已取消");
  }
  if (manager_ == nullptr) {
    LOG_ERROR("IEC61850同步命令服务未就绪");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850模块未就绪");
  }
  if (request == nullptr || response == nullptr) {
    LOG_ERROR("IEC61850同步命令请求或响应为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850同步命令请求或响应为空");
  }

  try {
    const auto cancellation =
        std::make_shared<std::atomic_bool>(false);
    std::jthread cancellationWatcher;
    if (context != nullptr) {
      cancellationWatcher = std::jthread(
          [context, cancellation](std::stop_token stopToken) {
            while (!stopToken.stop_requested() && !context->IsCancelled()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (context->IsCancelled()) {
              cancellation->store(true, std::memory_order_release);
            }
          });
    }
    auto effectiveRequest = *request;
    if (context != nullptr) {
      const auto deadline = context->deadline();
      const auto maxDeadline =
          std::chrono::system_clock::time_point::max();
      if (deadline != maxDeadline) {
        const auto now = std::chrono::system_clock::now();
        if (deadline <= now) {
          LOG_WARNING("IEC61850同步命令已超过gRPC截止时间");
          return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                              "IEC61850同步命令已超过gRPC截止时间");
        }
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - now);
        const auto remainingMs = static_cast<std::uint32_t>(std::min<
            std::int64_t>(std::numeric_limits<std::uint32_t>::max(),
                          std::max<std::int64_t>(1, remaining.count())));
        if (effectiveRequest.timeout_ms() == 0 ||
            effectiveRequest.timeout_ms() > remainingMs) {
          effectiveRequest.set_timeout_ms(remainingMs);
        }
      }
    }
    const auto status =
        manager_->ExecuteDataCenterCommand(effectiveRequest, response,
                                           cancellation);
    if (!status.ok()) {
      LOG_ERROR("IEC61850同步命令执行失败: 原因={}",
                status.error_message());
    }
    if (context != nullptr && context->IsCancelled()) {
      LOG_WARNING("IEC61850同步命令执行期间gRPC请求已取消");
      return grpc::Status(grpc::StatusCode::CANCELLED,
                          "IEC61850同步命令已取消");
    }
    return status;
  } catch (const std::exception& exception) {
    LOG_ERROR("IEC61850同步命令处理发生异常: 异常信息={}",
              exception.what());
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "IEC61850同步命令处理发生异常");
  } catch (...) {
    LOG_ERROR("IEC61850同步命令处理发生未知异常");
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "IEC61850同步命令处理发生未知异常");
  }
}

}  // namespace IEC61850
