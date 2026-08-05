#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850MmsTransport.h"

namespace IEC61850 {

using MmsUnconfirmedHandler =
    std::function<void(std::span<const std::uint8_t>)>;
using MmsCancellationPredicate = std::function<bool()>;
// 返回false表示调用方在发送线性化边界前已经取消，请求不得进入传输层。
using MmsRequestSentHandler = std::function<bool()>;

// 发送一个确认服务请求，分流期间交错到达的未确认PDU，并只返回invokeID匹配的响应。
grpc::Status ExchangeMmsConfirmedRequest(
    MmsTransport& transport, std::span<const std::uint8_t> request,
    std::vector<std::uint8_t>* response,
    const MmsUnconfirmedHandler& onUnconfirmed = {},
    std::optional<std::chrono::milliseconds> timeout = std::nullopt,
    const MmsCancellationPredicate& isCancelled = {},
    const MmsRequestSentHandler& onRequestSent = {});

}  // namespace IEC61850
