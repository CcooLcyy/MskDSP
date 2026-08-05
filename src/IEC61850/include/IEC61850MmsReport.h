#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850ProtocolStack.h"

namespace IEC61850 {

// InformationReport中一个DataSet成员的解码约束。
struct MmsReportMember {
  std::string dataRef;
  IEC61850Proto::FunctionalConstraint fc =
      IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED;
  // true表示该成员是IEC Quality，需要额外映射MmsQuality字段。
  bool quality = false;
};

// 根据启动计划核对并解码一份InformationReport所需的最小上下文。
struct MmsReportDecodePlan {
  // 输出到内部事件的RCB根引用，例如LD/LN$BR$name。
  std::string reportRef;
  // 报文中的RptID；为空时拒绝解码，避免跨RCB误收报告。
  std::string reportId;
  // 规范化DataSet引用，例如LD/LN$dataset。
  std::string dataSetRef;
  std::uint64_t confRev = 0;
  IEC61850Proto::SclOptionalFields optionalFields;
  std::vector<MmsReportMember> members;
};

// 解码MMS Unconfirmed-PDU中的InformationReport。
// 该函数只做报文校验和有界对象转换，不执行网络IO、日志输出或DataCenter调用。
grpc::Status DecodeMmsInformationReport(
    std::span<const std::uint8_t> input, const MmsReportDecodePlan& plan,
    MmsReportSegment* result);

}  // namespace IEC61850
