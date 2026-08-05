#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850MmsService.h"
#include "IEC61850ProtocolStack.h"

namespace IEC61850 {

enum class MmsDirectoryValidationStage : std::uint8_t {
  BASE = 0,
  DATASETS = 1,
  COMPLETE = 2,
};

// 负责协议栈连接后的目录、RCB和GI前置核对，不执行网络IO。
class MmsSessionContract {
public:
  explicit MmsSessionContract(const ProtocolIedPlan& plan);

  grpc::Status ValidateOnlineDirectory(
      const MmsOnlineDirectory& directory,
      MmsDirectoryValidationStage stage =
          MmsDirectoryValidationStage::COMPLETE);
  std::vector<MmsRcbActivationRequest> BuildRcbActivationRequests() const;
  grpc::Status MarkRcbEnabled(std::string_view rcbRef,
                              std::uint64_t configRevision);
  grpc::Status MarkGeneralInterrogationRequested(std::string_view rcbRef);
  // 只有明确GI原因、当前请求仍pending且完整覆盖DataSet的报告才能完成GI。
  grpc::Status MarkGeneralInterrogationComplete(
      const MmsReportEvent& report);
  bool GeneralInterrogationPending(std::string_view rcbRef) const;
  bool Ready() const;
  void ResetReadiness();

private:
  ProtocolIedPlan plan_;
  std::unordered_set<std::string> enabledRcbs_;
  std::unordered_set<std::string> requestedGiRcbs_;
  std::unordered_set<std::string> completedGiRcbs_;
  bool directoryValidated_ = false;
};

// 将规范化SCL的"Domain/路径"引用转换为MMS Domain对象名。
// DataAttribute中的点号按IEC 61850 MMS规则转换为美元号；已有美元号保持不变。
grpc::Status ParseMmsDomainObjectReference(std::string_view reference,
                                           MmsObjectName* objectName);

// 将分段报告按报告引用和序号合并；异常组会被丢弃并允许下一组继续。
class MmsReportAssembler {
public:
  explicit MmsReportAssembler(std::int64_t segmentTimeoutMs = 1500);
  ~MmsReportAssembler();

  std::optional<MmsReportEvent> Push(MmsReportSegment segment,
                                     std::int64_t nowMs);
  void Expire(std::int64_t nowMs);
  void Clear();

private:
struct PendingReport;
  std::int64_t segmentTimeoutMs_;
  std::vector<PendingReport> pending_;
};

}  // namespace IEC61850
