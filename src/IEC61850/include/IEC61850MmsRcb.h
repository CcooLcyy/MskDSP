#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <grpcpp/support/status.h>

#include "IEC61850MmsService.h"
#include "IEC61850ProtocolStack.h"

namespace IEC61850 {

// RCB写入阶段；配置字段必须在RptEna=false后写入，最后再启用。
enum class MmsRcbWritePhase : std::uint8_t {
  DISABLE = 0,
  CONFIGURE = 1,
  ENABLE = 2,
};

// 根据启动计划构造一个RCB阶段Write请求，不执行网络IO。
// CONFIGURE阶段固定按RptID、DatSet、OptFlds、BufTm、TrgOps、IntgPd顺序生成。
grpc::Status BuildMmsRcbWriteRequest(
    const MmsRcbActivationRequest& activation, MmsRcbWritePhase phase,
    MmsWriteRequest* request);

// 构造一次独立的$GI=true写入；该操作必须在RptEna启用后单独发送。
grpc::Status BuildMmsRcbGeneralInterrogationRequest(
    const MmsRcbActivationRequest& activation, MmsWriteRequest* request);

// 解码RCB根对象Read返回的Data.structure；调用方负责补充rcbRef和最大实例数。
// buffered决定BRCB/URCB中Resv字段的位置，所有可选尾部字段只做类型核对。
grpc::Status DecodeMmsRcbData(std::span<const std::uint8_t> encodedData,
                              bool buffered,
                              MmsDirectoryReportControl* result);

}  // namespace IEC61850
