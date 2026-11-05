#pragma once

#include <cstdint>
#include <format>
#include <string>

#include <grpcpp/support/status.h>

#include "DataCenter.pb.h"
#include "Logger.h"

namespace DataCenter::detail {
// 缺席和显式零具有不同语义，日志始终保留完整的三十二位品质位。
template <typename Message>
std::string FormatQualityFlags(const Message& message) {
  return message.has_quality_flags()
             ? std::format("0x{:08X}", message.quality_flags())
             : "未提供";
}

// 统一校验兼容摘要；未提供明细位的旧请求完全沿用原摘要。
template <typename Message>
grpc::Status NormalizeQuality(const Message& message,
                              DataCenterProto::Quality* quality) {
  *quality = message.quality();
  if (!message.has_quality_flags()) {
    return grpc::Status::OK;
  }
  const uint32_t flags = message.quality_flags();
  const auto summary = (flags & DataCenterProto::QUALITY_FLAG_INVALID) != 0
                           ? DataCenterProto::QUALITY_BAD
                       : flags != 0 ? DataCenterProto::QUALITY_UNCERTAIN
                                    : DataCenterProto::QUALITY_GOOD;
  if (*quality != DataCenterProto::QUALITY_UNSPECIFIED && *quality != summary) {
    LOG_WARNING("DataCenter 拒绝品质摘要与品质位冲突的请求: 摘要={}, 品质位={}, 应有摘要={}",
                static_cast<int>(*quality), FormatQualityFlags(message),
                static_cast<int>(summary));
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "品质摘要与品质位不一致");
  }
  constexpr uint32_t knownFlags =
      DataCenterProto::QUALITY_FLAG_OVERFLOW |
      DataCenterProto::QUALITY_FLAG_BLOCKED |
      DataCenterProto::QUALITY_FLAG_SUBSTITUTED |
      DataCenterProto::QUALITY_FLAG_NOT_TOPICAL |
      DataCenterProto::QUALITY_FLAG_INVALID;
  if ((flags & ~knownFlags) != 0) {
    LOG_WARNING("DataCenter 品质位包含未知位，完整保留并生成保守摘要: 品质位={}, 未知位=0x{:08X}, 摘要={}",
                FormatQualityFlags(message), flags & ~knownFlags,
                static_cast<int>(summary));
  }
  *quality = summary;
  return grpc::Status::OK;
}

// 构造新更新时完整替换品质，避免把缺席误写为零或残留上次品质位。
template <typename Source, typename Destination>
void CopyQuality(const Source& source, DataCenterProto::Quality quality,
                 Destination* destination) {
  destination->set_quality(quality);
  if (source.has_quality_flags()) {
    destination->set_quality_flags(source.quality_flags());
  } else {
    destination->clear_quality_flags();
  }
}
}  // namespace DataCenter::detail
