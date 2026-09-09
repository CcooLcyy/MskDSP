#include "BoardIOCommandService.hpp"

#include <chrono>

#include "BoardOutputManager.hpp"
#include "Logger.h"

namespace BoardIO {

BoardIOCommandService::BoardIOCommandService(
    BoardOutputManager* outputManager,
    const std::atomic<uint32_t>* outputConnectionId) :
  outputManager_(outputManager),
  outputConnectionId_(outputConnectionId) {}

grpc::Status BoardIOCommandService::ExecuteCommand(
    grpc::ServerContext* context,
    const DataCenterProto::ExecuteCommandRequest* request,
    DataCenterProto::ExecuteCommandResponse* response) {
  if (request == nullptr || response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "遥控请求或响应为空");
  }
  if (outputManager_ == nullptr || outputConnectionId_ == nullptr) {
    LOG_ERROR("BoardIO 遥控服务尚未绑定输出管理器");
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "板载输出管理器尚未就绪");
  }
  LOG_INFO("BoardIO 收到同步遥控命令: request_id={}，tag={}，conn_id={}",
           request->request_id(), request->dst().tag(),
           request->dst().conn_id());
  return outputManager_->ExecuteCommand(
      *request, outputConnectionId_->load(std::memory_order_acquire), response,
      [context]() {
        if (context == nullptr) {
          return grpc::Status::OK;
        }
        const auto deadline = context->deadline();
        if (deadline != std::chrono::system_clock::time_point::max() &&
            std::chrono::system_clock::now() >= deadline) {
          return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                              "BoardIO 遥控命令已超过截止时间");
        }
        if (context->IsCancelled()) {
          return grpc::Status(grpc::StatusCode::CANCELLED,
                              "BoardIO 遥控命令已取消");
        }
        return grpc::Status::OK;
      });
}

}  // namespace BoardIO
