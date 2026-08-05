#pragma once

#include <cstddef>
#include <cstdint>

namespace mskdsp {

// IEC61850完整目标态允许的最大gRPC消息大小。
inline constexpr int kIec61850MaxGrpcMessageBytes = 72 * 1024 * 1024;

// MMS点值发布管线的默认值和安全上限。
inline constexpr uint32_t kIec61850DefaultMmsEventQueueCapacity = 4096;
inline constexpr uint32_t kIec61850MaxMmsEventQueueCapacity = 65536;
inline constexpr uint32_t kIec61850DefaultPublishBatchSize = 256;
inline constexpr uint32_t kIec61850MaxPublishBatchSize = 4096;
inline constexpr uint32_t kIec61850DefaultPublishBatchWindowMs = 20;
inline constexpr uint32_t kIec61850MaxPublishBatchWindowMs = 1000;
inline constexpr std::size_t kIec61850MaxMmsVariableValueBytes =
    256 * 1024;
inline constexpr std::size_t kIec61850MaxMmsReportRetainedBytes =
    4 * 1024 * 1024;
inline constexpr std::size_t kIec61850MaxMmsPendingReportGroups = 64;
inline constexpr std::size_t kIec61850MaxMmsQueueRetainedBytes =
    16 * 1024 * 1024;
inline constexpr std::size_t kIec61850MaxMmsBatchSerializedBytes =
    3 * 1024 * 1024;

}  // namespace mskdsp
