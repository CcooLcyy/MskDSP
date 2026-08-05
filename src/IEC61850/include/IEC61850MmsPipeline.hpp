#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include "DataCenter.pb.h"
#include "IEC61850.pb.h"
#include "IEC61850DataCenterClient.h"
#include "IEC61850ProtocolStack.h"

namespace IEC61850 {

struct MmsPublishConfig {
  std::string connName;
  uint32_t connId = 0;
  IEC61850Proto::PointMappings mappings;
  std::size_t queueCapacity = 0;
  std::size_t batchSize = 0;
  std::chrono::milliseconds batchWindow{0};
};

class MmsEventPipeline {
public:
  explicit MmsEventPipeline(DataCenterClient* dataCenter);
  ~MmsEventPipeline();

  MmsEventPipeline(const MmsEventPipeline&) = delete;
  MmsEventPipeline& operator=(const MmsEventPipeline&) = delete;

  grpc::Status ConfigureIed(MmsPublishConfig config);
  grpc::Status ConfigureIed(MmsPublishConfig config,
                            uint64_t* activationToken);
  std::function<void(MmsReportEvent)> MakeReportCallback(
      std::string_view connName, uint64_t activationToken) const;
  void UpdateConnectionId(std::string_view connName, uint32_t connId);
  // 立即使报告入口和会话令牌失效，不等待在途DataCenter调用。
  grpc::Status InvalidateIed(std::string_view connName);
  // 等待已失效入口的在途DataCenter调用收敛。
  grpc::Status WaitForDeactivation(std::string_view connName);
  grpc::Status DeactivateIed(std::string_view connName);
  void RemoveIed(std::string_view connName);
  bool EnqueueReport(std::string_view connName, MmsReportEvent report);
  IEC61850Proto::RuntimeStatistics GetStatistics(
      std::string_view connName) const;
  bool WaitUntilIdle(std::chrono::milliseconds timeout) const;

private:
  enum class ConversionResult {
    PUBLISHED,
    DEADBAND_FILTERED,
    TYPE_MISMATCH,
    INVALID_VALUE,
  };

  struct PublishedPointState {
    bool hasNumericValue = false;
    long double numericValue = 0.0L;
    bool hasQuality = false;
    DataCenterProto::Quality quality = DataCenterProto::QUALITY_UNSPECIFIED;
  };

  struct MappingState {
    IEC61850Proto::PointMapping mapping;
    PublishedPointState published;
  };

  struct QueuedReport {
    MmsReportEvent report;
    std::size_t nextValue = 0;
    std::size_t retainedBytes = 0;
  };

  struct IedPipelineState {
    mutable std::mutex mutex;
    mutable std::condition_variable_any condition;
    uint32_t connId = 0;
    std::size_t queueCapacity = 0;
    std::size_t batchSize = 0;
    std::chrono::milliseconds batchWindow{0};
    bool active = false;
    bool publishing = false;
    uint64_t generation = 0;
    uint64_t activationToken = 0;
    std::size_t queuedValueCount = 0;
    std::size_t queuedRetainedBytes = 0;
    grpc::ClientContext* inFlightContext = nullptr;
    std::unordered_map<std::string, MappingState> mappings;
    std::deque<QueuedReport> queue;
    IEC61850Proto::RuntimeStatistics statistics;
    std::jthread worker;
  };

  struct BatchBuildState {
    uint64_t generation = 0;
    std::unordered_map<std::string, PublishedPointState> candidates;
    bool logUnmapped = false;
    bool logTypeMismatch = false;
    bool logInvalid = false;
    bool logOversized = false;
    std::size_t serializedBytes = 0;
    std::string firstUnmappedRef;
    std::string firstTypeMismatchRef;
    std::string firstInvalidRef;
    std::string firstOversizedRef;
  };

  static std::string MappingKey(std::string_view dataRef,
                                IEC61850Proto::FunctionalConstraint fc);
  static int64_t CurrentTimestampMs();
  static DataCenterProto::Quality ConvertQuality(const MmsQuality& quality,
                                                 bool timestampValid);
  static ConversionResult ConvertValue(
      const MmsDataValue& source, DataCenterProto::Quality quality,
      const MappingState& mapping, PublishedPointState* candidate,
      DataCenterProto::PointValue* value);
  static bool EnqueueReportForState(
      const std::string& connName,
      const std::shared_ptr<IedPipelineState>& state,
      uint64_t activationToken, MmsReportEvent report);
  std::shared_ptr<IedPipelineState> FindState(
      std::string_view connName) const;
  void EnsureWorkerLocked(const std::string& connName,
                          const std::shared_ptr<IedPipelineState>& state);
  static std::size_t QueuedValueCountLocked(
      const IedPipelineState& state);
  static void BuildBatchLocked(IedPipelineState* state,
                               DataCenterProto::BatchPublishRequest* batch,
                               BatchBuildState* buildState);
  static void LogBatchDiagnostics(const std::string& connName,
                                  const BatchBuildState& buildState) noexcept;
  void WorkerLoop(std::string connName,
                  std::shared_ptr<IedPipelineState> state,
                  std::stop_token stopToken);
  static void StopWorker(const std::shared_ptr<IedPipelineState>& state);

  DataCenterClient* dataCenter_ = nullptr;
  mutable std::mutex statesMutex_;
  std::unordered_map<std::string, std::shared_ptr<IedPipelineState>> states_;
};

}  // namespace IEC61850
