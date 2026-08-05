#include "IEC61850MmsJournal.h"

#include <algorithm>
#include <array>
#include <limits>

namespace IEC61850 {
namespace {

grpc::Status Invalid(std::string_view reason) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      std::string("IEC61850 MMS Journal参数无效: ") +
                          std::string(reason));
}

grpc::Status Deadline(std::chrono::milliseconds timeout,
                      const MmsCancellationPredicate& cancelled) {
  if (cancelled && cancelled()) {
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS Journal操作已取消");
  }
  if (timeout <= std::chrono::milliseconds::zero()) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS Journal操作超时");
  }
  return grpc::Status::OK;
}

grpc::Status CheckDeadlineAt(
    std::chrono::steady_clock::time_point deadline,
    const MmsCancellationPredicate& cancelled) {
  if (cancelled && cancelled()) {
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "IEC61850 MMS Journal操作已取消");
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 MMS Journal操作超时");
  }
  return grpc::Status::OK;
}

std::chrono::milliseconds Remaining(
    std::chrono::steady_clock::time_point deadline) {
  return std::max(std::chrono::milliseconds(1),
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      deadline - std::chrono::steady_clock::now()));
}

}  // namespace

grpc::Status MmsJournalClient::CheckArguments(
    const MmsFileExchange& exchange, const std::uint32_t* nextInvokeId,
    std::chrono::milliseconds timeout) {
  if (!exchange || nextInvokeId == nullptr || *nextInvokeId == 0) {
    return Invalid("Journal事务交换器或invokeID未配置");
  }
  if (timeout <= std::chrono::milliseconds::zero()) {
    return Invalid("Journal超时参数无效");
  }
  return grpc::Status::OK;
}

grpc::Status MmsJournalClient::Advance(std::uint32_t* invokeId) {
  if (invokeId == nullptr || *invokeId == 0) {
    return Invalid("Journal invokeID无效");
  }
  *invokeId = *invokeId == std::numeric_limits<std::uint32_t>::max()
                  ? 1
                  : *invokeId + 1;
  return grpc::Status::OK;
}

grpc::Status MmsJournalClient::Exchange(
    std::span<const std::uint8_t> request, std::vector<std::uint8_t>* response,
    std::chrono::milliseconds timeout,
    const MmsCancellationPredicate& isCancelled) const {
  if (response == nullptr) {
    return Invalid("Journal响应输出为空");
  }
  auto status = Deadline(timeout, isCancelled);
  if (!status.ok()) {
    return status;
  }
  return exchange_(request, response, timeout, isCancelled, [] { return true; });
}

grpc::Status MmsJournalClient::JournalStatus(
    const MmsJournalStatusRequest& request,
    MmsJournalStatusResponse* response, std::chrono::milliseconds timeout,
    const MmsCancellationPredicate& isCancelled) const {
  auto status = CheckArguments(exchange_, nextInvokeId_, timeout);
  if (!status.ok() || response == nullptr) {
    return response == nullptr ? Invalid("JournalStatus响应输出为空") : status;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<std::uint8_t, 65536> encoded{};
  std::size_t size = 0;
  status = EncodeMmsJournalStatusRequest(*nextInvokeId_, request, encoded,
                                          &size);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> received;
  status = Exchange(std::span<const std::uint8_t>(encoded.data(), size),
                    &received, Remaining(deadline), isCancelled);
  if (!status.ok()) return status;
  status = CheckDeadlineAt(deadline, isCancelled);
  if (!status.ok()) return status;
  status = DecodeMmsJournalStatusResponse(received, *nextInvokeId_, response);
  if (!status.ok()) return status;
  return Advance(nextInvokeId_);
}

grpc::Status MmsJournalClient::InitializeJournal(
    const MmsInitializeJournalRequest& request,
    MmsInitializeJournalResponse* response, std::chrono::milliseconds timeout,
    const MmsCancellationPredicate& isCancelled) const {
  auto status = CheckArguments(exchange_, nextInvokeId_, timeout);
  if (!status.ok() || response == nullptr) {
    return response == nullptr ? Invalid("InitializeJournal响应输出为空")
                               : status;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<std::uint8_t, 65536> encoded{};
  std::size_t size = 0;
  status = EncodeMmsInitializeJournalRequest(*nextInvokeId_, request, encoded,
                                              &size);
  if (!status.ok()) return status;
  std::vector<std::uint8_t> received;
  status = Exchange(std::span<const std::uint8_t>(encoded.data(), size),
                    &received, Remaining(deadline), isCancelled);
  if (!status.ok()) return status;
  status = CheckDeadlineAt(deadline, isCancelled);
  if (!status.ok()) return status;
  status = DecodeMmsInitializeJournalResponse(received, *nextInvokeId_,
                                              response);
  if (!status.ok()) return status;
  return Advance(nextInvokeId_);
}

grpc::Status MmsJournalClient::ReadJournal(
    const MmsJournalReadRequest& request, MmsJournalReadResponse* response,
    std::size_t maxEntries, std::chrono::milliseconds timeout,
    const MmsCancellationPredicate& isCancelled) const {
  auto status = CheckArguments(exchange_, nextInvokeId_, timeout);
  if (!status.ok() || response == nullptr) {
    return response == nullptr ? Invalid("ReadJournal响应输出为空") : status;
  }
  if (maxEntries == 0 || maxEntries > 4096) {
    return Invalid("ReadJournal条目上限无效");
  }
  *response = {};
  MmsJournalReadRequest page = request;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    const auto remaining = Remaining(deadline);
    status = Deadline(remaining, isCancelled);
    if (!status.ok()) return status;
    if (response->entries.size() >= maxEntries) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "ReadJournal条目超过调用方上限");
    }
    std::array<std::uint8_t, 65536> encoded{};
    std::size_t size = 0;
    status = EncodeMmsJournalReadRequest(*nextInvokeId_, page, encoded, &size);
    if (!status.ok()) return status;
    std::vector<std::uint8_t> received;
    status = Exchange(std::span<const std::uint8_t>(encoded.data(), size),
                      &received, remaining, isCancelled);
    if (!status.ok()) return status;
    status = CheckDeadlineAt(deadline, isCancelled);
    if (!status.ok()) return status;
    MmsJournalReadResponse decoded;
    status = DecodeMmsJournalReadResponse(received, *nextInvokeId_, &decoded);
    if (!status.ok()) return status;
    if (decoded.entries.empty() && decoded.moreFollows) {
      return grpc::Status(grpc::StatusCode::DATA_LOSS,
                          "ReadJournal分页为空且moreFollows仍为true");
    }
    if (decoded.entries.size() > maxEntries - response->entries.size()) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "ReadJournal累计条目超过调用方上限");
    }
    response->entries.insert(response->entries.end(), decoded.entries.begin(),
                             decoded.entries.end());
    response->moreFollows = decoded.moreFollows;
    status = Advance(nextInvokeId_);
    if (!status.ok() || !decoded.moreFollows) return status;
    const auto& last = decoded.entries.back();
    if (last.entryId.empty() ||
        (!page.startAfterEntryId.empty() &&
         page.startAfterEntryId == last.entryId)) {
      return grpc::Status(grpc::StatusCode::DATA_LOSS,
                          "ReadJournal分页EntryID未前进");
    }
    page.startAfterEntryId = last.entryId;
    page.startAfterTimeMs = last.occurrenceTimeMs;
  }
}

}  // namespace IEC61850
