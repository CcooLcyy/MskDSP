#include "BoardOutputManager.hpp"

#include <array>
#include <format>
#include <string_view>
#include <utility>

#include "Logger.h"

namespace BoardIO {
namespace {

constexpr uint32_t kDo1Offset = 256;
constexpr uint32_t kDo2Offset = 39;
constexpr uint32_t kPowerLossRelayOffset = 257;
constexpr std::string_view kModuleName = "BoardIO";
constexpr std::string_view kOutputConnectionName = "board-do";

constexpr std::array<GpioOutputValue, 3> kInitialValues = {{
    {.offset = kDo1Offset, .value = false},
    {.offset = kDo2Offset, .value = false},
    {.offset = kPowerLossRelayOffset, .value = true},
}};

constexpr std::array<GpioOutputValue, 3> kStoppedValues = {{
    {.offset = kDo1Offset, .value = false},
    {.offset = kDo2Offset, .value = false},
    {.offset = kPowerLossRelayOffset, .value = false},
}};

uint32_t OffsetForTag(std::string_view tag) {
  if (tag == "DO1") {
    return kDo1Offset;
  }
  if (tag == "DO2") {
    return kDo2Offset;
  }
  return 0;
}

}  // namespace

BoardOutputManager::BoardOutputManager() :
  BoardOutputManager(std::make_unique<GpioOutputDriver>()) {}

BoardOutputManager::BoardOutputManager(
    std::unique_ptr<GpioOutputDriver> driver) :
  driver_(std::move(driver)) {
  if (!driver_) {
    driver_ = std::make_unique<GpioOutputDriver>();
  }
}

BoardOutputManager::~BoardOutputManager() {
  std::string error;
  if (!Stop(&error) && !error.empty()) {
    LOG_ERROR("BoardIO 析构时停止板载输出失败: {}", error);
  }
}

bool BoardOutputManager::Start(const std::string& chipPath,
                               std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  std::lock_guard lock(mutex_);
  if (ready_) {
    LOG_INFO("BoardIO 板载输出功能已经启动，忽略重复启动请求");
    return true;
  }

  const std::vector<GpioOutputValue> initialValues(kInitialValues.begin(),
                                                    kInitialValues.end());
  if (!driver_->Open(chipPath, initialValues, error)) {
    LOG_ERROR("BoardIO 申请板载输出 GPIO 失败: {}",
              error == nullptr ? "未提供错误信息" : *error);
    return false;
  }
  ready_ = true;
  LOG_INFO("BoardIO 板载输出功能已启动: device={}，DO1(GPIO256)=0，DO2(GPIO39)=0，失电继电器(GPIO257)=1",
           chipPath);
  return true;
}

bool BoardOutputManager::Stop(std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  std::lock_guard lock(mutex_);
  if (!ready_) {
    return driver_->Close(error);
  }

  ready_ = false;
  bool success = true;
  std::string firstError;
  for (const auto& item : kStoppedValues) {
    std::string writeError;
    if (!driver_->SetValue(item.offset, item.value, &writeError)) {
      success = false;
      if (firstError.empty()) {
        firstError = writeError;
      }
      LOG_ERROR("BoardIO 停止时拉低 GPIO 失败: offset={}，原因={}",
                item.offset, writeError);
    } else {
      LOG_INFO("BoardIO 停止时已拉低 GPIO: offset={}", item.offset);
    }
  }

  std::string closeError;
  if (!driver_->Close(&closeError)) {
    success = false;
    if (firstError.empty()) {
      firstError = closeError;
    }
    LOG_ERROR("BoardIO 释放板载输出 GPIO 失败: {}", closeError);
  } else {
    LOG_INFO("BoardIO 板载输出 GPIO 已释放");
  }
  if (!success && error != nullptr) {
    *error = std::move(firstError);
  }
  return success;
}

bool BoardOutputManager::Ready() const {
  std::lock_guard lock(mutex_);
  return ready_;
}

grpc::Status BoardOutputManager::ExecuteCommand(
    const DataCenterProto::ExecuteCommandRequest& request,
    uint32_t outputConnId,
    DataCenterProto::ExecuteCommandResponse* response,
    std::function<grpc::Status()> cancellationCheck) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  response->Clear();
  if (cancellationCheck) {
    const auto status = cancellationCheck();
    if (!status.ok()) {
      LOG_WARNING("BoardIO 遥控命令在等待输出锁前已取消: tag={}，原因={}",
                  request.dst().tag(), status.error_message());
      return status;
    }
  }
  if (!request.has_dst()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "命令目的端点为空");
  }
  *response->mutable_dst() = request.dst();

  if (request.dst().module_name() != kModuleName ||
      request.dst().conn_name() != kOutputConnectionName) {
    Reject(response, DataCenterProto::COMMAND_REJECT_BAD_CONFIG,
           "命令目的连接与 BoardIO/board-do 不一致");
    LOG_ERROR("BoardIO 拒绝目的连接不匹配的遥控命令: module={}，conn={}，conn_id={}",
              request.dst().module_name(), request.dst().conn_name(),
              request.dst().conn_id());
    return grpc::Status::OK;
  }
  if (outputConnId == 0) {
    Unavailable(response, "DataCenter 遥控连接尚未注册");
    LOG_ERROR("BoardIO 无法执行遥控命令，DataCenter 遥控连接尚未注册: tag={}",
              request.dst().tag());
    return grpc::Status::OK;
  }
  if (request.dst().conn_id() != outputConnId) {
    Reject(response, DataCenterProto::COMMAND_REJECT_BAD_CONFIG,
           "命令目的 conn_id 与当前 board-do 不一致");
    LOG_ERROR("BoardIO 拒绝 conn_id 不匹配的遥控命令: conn_id={}，expected_conn_id={}",
              request.dst().conn_id(), outputConnId);
    return grpc::Status::OK;
  }

  const uint32_t offset = OffsetForTag(request.dst().tag());
  if (offset == 0) {
    Reject(response, DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT,
           "遥控点仅支持 DO1 或 DO2");
    LOG_ERROR("BoardIO 拒绝未知遥控点: tag={}", request.dst().tag());
    return grpc::Status::OK;
  }
  if (request.value().kind_case() !=
      DataCenterProto::PointValue::kBoolValue) {
    Reject(response, DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT,
           "DO 遥控值必须为 BOOL 类型");
    LOG_ERROR("BoardIO 拒绝非 BOOL 遥控值: tag={}，value_kind={}",
              request.dst().tag(),
              static_cast<int>(request.value().kind_case()));
    return grpc::Status::OK;
  }

  const bool requestedValue = request.value().bool_value();
  response->set_requested_value(requestedValue ? 1.0 : 0.0);
  std::lock_guard lock(mutex_);
  if (cancellationCheck) {
    const auto status = cancellationCheck();
    if (!status.ok()) {
      LOG_WARNING("BoardIO 遥控命令在获得输出锁后已取消，不写入 GPIO: tag={}，原因={}",
                  request.dst().tag(), status.error_message());
      return status;
    }
  }
  if (!ready_) {
    Unavailable(response, "板载输出 GPIO 尚未准备完成");
    LOG_ERROR("BoardIO 无法执行遥控命令，板载输出尚未准备: tag={}，value={}",
              request.dst().tag(), requestedValue);
    return grpc::Status::OK;
  }

  std::string error;
  if (!driver_->SetValue(offset, requestedValue, &error)) {
    Unavailable(response, std::format("GPIO 写入失败: {}", error));
    LOG_ERROR("BoardIO 遥控 GPIO 写入失败: tag={}，offset={}，value={}，原因={}",
              request.dst().tag(), offset, requestedValue, error);
    return grpc::Status::OK;
  }

  response->mutable_dst()->set_module_name(std::string(kModuleName));
  response->mutable_dst()->set_conn_name(std::string(kOutputConnectionName));
  response->mutable_dst()->set_conn_id(outputConnId);
  response->set_status(DataCenterProto::COMMAND_ACCEPTED);
  response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
  response->set_accepted_value(requestedValue ? 1.0 : 0.0);
  LOG_INFO("BoardIO 遥控 GPIO 写入成功: tag={}，offset={}，value={}",
           request.dst().tag(), offset, requestedValue);
  return grpc::Status::OK;
}

void BoardOutputManager::Reject(
    DataCenterProto::ExecuteCommandResponse* response,
    DataCenterProto::CommandRejectCode rejectCode,
    std::string reason) {
  response->set_status(DataCenterProto::COMMAND_REJECTED);
  response->set_reject_code(rejectCode);
  response->set_reason(std::move(reason));
}

void BoardOutputManager::Unavailable(
    DataCenterProto::ExecuteCommandResponse* response, std::string reason) {
  response->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
  response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
  response->set_reason(std::move(reason));
}

}  // namespace BoardIO
