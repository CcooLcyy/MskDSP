#pragma once

#include <chrono>
#include <functional>

#include "IEC61850MmsFile.h"

namespace IEC61850 {

using MmsDynamicDataSetValidator =
    std::function<grpc::Status(const MmsNamedVariableListDefinition&)>;
using MmsDynamicDataSetRcbBinder =
    std::function<grpc::Status(const MmsObjectName&)>;

// 动态DataSet/NVL事务。创建成功后先核对远端成员，再绑定RCB；绑定失败
// 必须删除刚创建的NVL，避免会话重连后遗留不可引用的运行时对象。
class MmsDynamicDataSetClient {
public:
  MmsDynamicDataSetClient() = default;
  MmsDynamicDataSetClient(MmsFileExchange exchange, std::uint32_t* nextInvokeId)
      : exchange_(std::move(exchange)), nextInvokeId_(nextInvokeId) {}

  void SetCapabilities(bool defineNamedVariableList,
                       bool deleteNamedVariableList) noexcept {
    supportsDefine_ = defineNamedVariableList;
    supportsDelete_ = deleteNamedVariableList;
  }

  grpc::Status Create(
      const MmsNamedVariableListDefinition& definition,
      const MmsDynamicDataSetValidator& validateRemote = {},
      const MmsDynamicDataSetRcbBinder& bindRcb = {},
      std::chrono::milliseconds timeout = std::chrono::seconds(5),
      const MmsCancellationPredicate& isCancelled = {}) const;
  grpc::Status Delete(
      const MmsObjectName& listName,
      std::chrono::milliseconds timeout = std::chrono::seconds(5),
      const MmsCancellationPredicate& isCancelled = {}) const;

private:
  grpc::Status Exchange(std::span<const std::uint8_t> request,
                        std::vector<std::uint8_t>* response,
                        std::chrono::milliseconds timeout,
                        const MmsCancellationPredicate& isCancelled) const;
  grpc::Status DeleteCreated(const MmsObjectName& listName,
                             std::chrono::steady_clock::time_point deadline) const;
  static grpc::Status Advance(std::uint32_t* invokeId);

  MmsFileExchange exchange_;
  std::uint32_t* nextInvokeId_ = nullptr;
  bool supportsDefine_ = false;
  bool supportsDelete_ = false;
};

}  // namespace IEC61850
