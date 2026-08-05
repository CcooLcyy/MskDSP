#include "IEC61850MmsDynamicDataSet.h"

#include <algorithm>
#include <array>
#include <limits>

namespace IEC61850 {
namespace {

grpc::Status Invalid(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      std::string("IEC61850 MMS动态DataSet参数无效: ") +
                          std::string(reason));
}

grpc::Status Aborted(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::ABORTED,
                      std::string("IEC61850 MMS动态DataSet状态不确定: ") +
                          std::string(reason));
}

std::chrono::milliseconds Remaining(std::chrono::steady_clock::time_point deadline) {
  return std::max(std::chrono::milliseconds(1),
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      deadline - std::chrono::steady_clock::now()));
}

}  // namespace

grpc::Status MmsDynamicDataSetClient::Advance(std::uint32_t* invokeId) {
  if (invokeId == nullptr || *invokeId == 0) return Invalid("invokeID无效");
  *invokeId = *invokeId == std::numeric_limits<std::uint32_t>::max()
                  ? 1
                  : *invokeId + 1;
  return grpc::Status::OK;
}

grpc::Status MmsDynamicDataSetClient::Exchange(
    std::span<const std::uint8_t> request, std::vector<std::uint8_t>* response,
    std::chrono::milliseconds timeout,
    const MmsCancellationPredicate& isCancelled) const {
  if (!exchange_ || response == nullptr || timeout <= std::chrono::milliseconds::zero()) {
    return Invalid("动态DataSet交换参数无效");
  }
  if (isCancelled && isCancelled()) {
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS动态DataSet操作已取消");
  }
  return exchange_(request, response, timeout, isCancelled, [] { return true; });
}

grpc::Status MmsDynamicDataSetClient::Create(
    const MmsNamedVariableListDefinition& definition,
    const MmsDynamicDataSetValidator& validateRemote,
    const MmsDynamicDataSetRcbBinder& bindRcb,
    std::chrono::milliseconds timeout,
    const MmsCancellationPredicate& isCancelled) const {
  if (!supportsDefine_) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "MMS服务端未协商DefineNamedVariableList");
  }
  if (!exchange_ || nextInvokeId_ == nullptr || timeout <= std::chrono::milliseconds::zero()) {
    return Invalid("动态DataSet事务参数无效");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<std::uint8_t, 65536> encoded{};
  std::size_t size = 0;
  auto status = EncodeMmsDefineNamedVariableListRequest(*nextInvokeId_, definition,
                                                        encoded, &size);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> response;
  status = Exchange(std::span<const std::uint8_t>(encoded.data(), size), &response,
                    Remaining(deadline), isCancelled);
  if (!status.ok()) return status;
  if (isCancelled && isCancelled()) {
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS动态DataSet操作已取消");
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS动态DataSet操作超时");
  }
  MmsConfirmedPduView pdu;
  status = DecodeMmsConfirmedResponse(response, &pdu);
  if (!status.ok() || pdu.invokeId != *nextInvokeId_ || pdu.serviceTag != 11 ||
      !pdu.serviceValue.empty()) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "DefineNamedVariableList响应无效");
  }
  status = Advance(nextInvokeId_);
  if (!status.ok()) return status;
  if (validateRemote) {
    status = validateRemote(definition);
    if (!status.ok()) {
      const auto rollback = DeleteCreated(definition.listName, deadline);
      if (!rollback.ok()) return Aborted("创建后校验失败且回滚删除失败");
      return status;
    }
  }
  if (bindRcb) {
    status = bindRcb(definition.listName);
    if (!status.ok()) {
      const auto rollback = DeleteCreated(definition.listName, deadline);
      if (!rollback.ok()) return Aborted("RCB绑定失败且回滚删除失败");
      return status;
    }
  }
  return grpc::Status::OK;
}

grpc::Status MmsDynamicDataSetClient::DeleteCreated(
    const MmsObjectName& listName,
    std::chrono::steady_clock::time_point deadline) const {
  if (!supportsDelete_) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "MMS服务端未协商DeleteNamedVariableList");
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS动态DataSet回滚超时");
  }
  std::array<std::uint8_t, 65536> encoded{};
  std::size_t size = 0;
  auto status = EncodeMmsDeleteNamedVariableListRequest(*nextInvokeId_, listName,
                                                        encoded, &size);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> response;
  status = Exchange(std::span<const std::uint8_t>(encoded.data(), size), &response,
                    Remaining(deadline), {});
  if (!status.ok()) return status;
  if (std::chrono::steady_clock::now() >= deadline) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS动态DataSet回滚超时");
  }
  MmsConfirmedPduView pdu;
  status = DecodeMmsConfirmedResponse(response, &pdu);
  if (!status.ok() || pdu.invokeId != *nextInvokeId_ || pdu.serviceTag != 13 ||
      !pdu.serviceValue.empty()) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "DeleteNamedVariableList回滚响应无效");
  }
  return Advance(nextInvokeId_);
}

grpc::Status MmsDynamicDataSetClient::Delete(
    const MmsObjectName& listName, std::chrono::milliseconds timeout,
    const MmsCancellationPredicate& isCancelled) const {
  if (!supportsDelete_) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "MMS服务端未协商DeleteNamedVariableList");
  }
  if (!exchange_ || nextInvokeId_ == nullptr || timeout <= std::chrono::milliseconds::zero()) {
    return Invalid("动态DataSet删除事务参数无效");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<std::uint8_t, 65536> encoded{};
  std::size_t size = 0;
  auto status = EncodeMmsDeleteNamedVariableListRequest(*nextInvokeId_, listName,
                                                        encoded, &size);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> response;
  status = Exchange(std::span<const std::uint8_t>(encoded.data(), size), &response,
                    Remaining(deadline), isCancelled);
  if (!status.ok()) return status;
  if (isCancelled && isCancelled()) {
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS动态DataSet删除操作已取消");
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS动态DataSet删除操作超时");
  }
  MmsConfirmedPduView pdu;
  status = DecodeMmsConfirmedResponse(response, &pdu);
  if (!status.ok() || pdu.invokeId != *nextInvokeId_ || pdu.serviceTag != 13 ||
      !pdu.serviceValue.empty()) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "DeleteNamedVariableList响应无效");
  }
  return Advance(nextInvokeId_);
}

}  // namespace IEC61850
