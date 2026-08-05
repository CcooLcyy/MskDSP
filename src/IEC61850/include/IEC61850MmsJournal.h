#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "IEC61850MmsFile.h"

namespace IEC61850 {

// Journal事务客户端复用现有MMS确认交换器；交换器负责当前A/B通道、
// Session代际和串行队列，Journal客户端只维护服务分页和有界输出。
class MmsJournalClient {
public:
  MmsJournalClient() = default;
  MmsJournalClient(MmsFileExchange exchange, std::uint32_t* nextInvokeId)
      : exchange_(std::move(exchange)), nextInvokeId_(nextInvokeId) {}

  grpc::Status JournalStatus(
      const MmsJournalStatusRequest& request,
      MmsJournalStatusResponse* response,
      std::chrono::milliseconds timeout = std::chrono::seconds(5),
      const MmsCancellationPredicate& isCancelled = {}) const;
  grpc::Status InitializeJournal(
      const MmsInitializeJournalRequest& request,
      MmsInitializeJournalResponse* response,
      std::chrono::milliseconds timeout = std::chrono::seconds(5),
      const MmsCancellationPredicate& isCancelled = {}) const;
  grpc::Status ReadJournal(
      const MmsJournalReadRequest& request, MmsJournalReadResponse* response,
      std::size_t maxEntries = 4096,
      std::chrono::milliseconds timeout = std::chrono::seconds(5),
      const MmsCancellationPredicate& isCancelled = {}) const;

private:
  grpc::Status Exchange(std::span<const std::uint8_t> request,
                        std::vector<std::uint8_t>* response,
                        std::chrono::milliseconds timeout,
                        const MmsCancellationPredicate& isCancelled) const;
  static grpc::Status CheckArguments(const MmsFileExchange& exchange,
                                     const std::uint32_t* nextInvokeId,
                                     std::chrono::milliseconds timeout);
  static grpc::Status Advance(std::uint32_t* invokeId);

  MmsFileExchange exchange_;
  std::uint32_t* nextInvokeId_ = nullptr;
};

}  // namespace IEC61850
