#include "IEC61850MmsPipeline.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <limits>
#include <utility>
#include <vector>

#include "Logger.h"
#include "ThreadUtil.hpp"
#include "mskdsp/IEC61850Limits.hpp"

namespace IEC61850 {
namespace {

constexpr auto kDeactivateWaitTimeout = std::chrono::milliseconds(1500);

bool ShouldLogCounter(uint64_t value) {
  return value == 1 || (value != 0 && (value & (value - 1)) == 0);
}

bool ShouldLogDropTransition(uint64_t previous, uint64_t current) {
  if (current <= previous) {
    return false;
  }
  if (previous == 0) {
    return true;
  }
  uint64_t nextPower = 1;
  while (nextPower <= previous &&
         nextPower <= (std::numeric_limits<uint64_t>::max() / 2)) {
    nextPower <<= 1;
  }
  return current >= nextPower;
}

std::size_t SaturatingAdd(std::size_t left, std::size_t right) noexcept {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return std::numeric_limits<std::size_t>::max();
  }
  return left + right;
}

std::size_t MmsValueBytes(const MmsValue& value) noexcept {
  if (const auto* text = std::get_if<std::string>(&value)) {
    return text->size();
  }
  if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&value)) {
    return bytes->size();
  }
  if (const auto* composite =
          std::get_if<std::shared_ptr<MmsCompositeValue>>(&value)) {
    if (*composite == nullptr) {
      return 0;
    }
    if (!(*composite)->encodedContent.empty()) {
      return (*composite)->encodedContent.size();
    }
    std::size_t total = 0;
    for (const auto& element : (*composite)->elements) {
      total = SaturatingAdd(total, MmsValueBytes(element));
    }
    return total;
  }
  return 0;
}

std::size_t VariableValueBytes(const MmsDataValue& value) noexcept {
  return MmsValueBytes(value.value);
}

std::size_t CountOversizedVariableValues(
    const MmsReportEvent& report) noexcept {
  return static_cast<std::size_t>(std::count_if(
      report.values.begin(), report.values.end(), [](const auto& value) {
        return VariableValueBytes(value) >
               mskdsp::kIec61850MaxMmsVariableValueBytes;
      }));
}

std::size_t EstimateReportRetainedBytes(
    const MmsReportEvent& report) noexcept {
  constexpr std::size_t kReportFixedBytes = 128;
  constexpr std::size_t kValueFixedBytes = 128;
  std::size_t retained = kReportFixedBytes;
  retained = SaturatingAdd(retained, report.reportRef.size());
  retained = SaturatingAdd(retained, report.dataSetRef.size());
  for (const auto& value : report.values) {
    retained = SaturatingAdd(retained, kValueFixedBytes);
    retained = SaturatingAdd(retained, value.dataRef.size());
    retained = SaturatingAdd(retained, VariableValueBytes(value));
  }
  return retained;
}

std::size_t VarintSize(std::size_t value) noexcept {
  std::size_t bytes = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++bytes;
  }
  return bytes;
}

std::size_t SerializedBatchPointBytes(
    const DataCenterProto::PublishRequest& point) noexcept {
  const auto pointBytes = point.ByteSizeLong();
  return SaturatingAdd(SaturatingAdd(1, VarintSize(pointBytes)), pointBytes);
}

bool WouldExceed(std::size_t current, std::size_t incoming,
                 std::size_t limit) noexcept {
  return incoming > limit || current > limit - incoming;
}

}  // namespace

MmsEventPipeline::MmsEventPipeline(DataCenterClient* dataCenter) :
  dataCenter_(dataCenter) {}

MmsEventPipeline::~MmsEventPipeline() {
  std::vector<std::shared_ptr<IedPipelineState>> states;
  {
    std::lock_guard lock(statesMutex_);
    states.reserve(states_.size());
    for (auto& [_, state] : states_) {
      states.emplace_back(std::move(state));
    }
    states_.clear();
  }
  for (const auto& state : states) {
    StopWorker(state);
  }
}

grpc::Status MmsEventPipeline::ConfigureIed(MmsPublishConfig config) {
  return ConfigureIed(std::move(config), nullptr);
}

grpc::Status MmsEventPipeline::ConfigureIed(MmsPublishConfig config,
                                            uint64_t* activationToken) {
  if (dataCenter_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "MMS发布管线未配置DataCenter客户端");
  }
  if (config.connName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "MMS发布管线conn_name不能为空");
  }
  if (!config.mappings.conn_name().empty() &&
      config.mappings.conn_name() != config.connName) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "MMS发布管线点映射conn_name不一致");
  }
  if (config.queueCapacity >
      mskdsp::kIec61850MaxMmsEventQueueCapacity) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "MMS待处理点值容量超过安全上限");
  }
  if (config.batchSize > mskdsp::kIec61850MaxPublishBatchSize) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "DataCenter单批发布点数超过安全上限");
  }
  if (config.batchWindow.count() < 0 ||
      config.batchWindow.count() >
          mskdsp::kIec61850MaxPublishBatchWindowMs) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "DataCenter合批窗口超出允许范围");
  }
  if (config.queueCapacity == 0) {
    config.queueCapacity = mskdsp::kIec61850DefaultMmsEventQueueCapacity;
  }
  if (config.batchSize == 0) {
    config.batchSize = mskdsp::kIec61850DefaultPublishBatchSize;
  }
  if (config.batchWindow.count() == 0) {
    config.batchWindow = std::chrono::milliseconds(
        mskdsp::kIec61850DefaultPublishBatchWindowMs);
  }

  std::shared_ptr<IedPipelineState> state;
  {
    std::lock_guard lock(statesMutex_);
    auto& slot = states_[config.connName];
    if (!slot) {
      slot = std::make_shared<IedPipelineState>();
    }
    state = slot;
  }

  std::size_t mappingCount = 0;
  uint64_t configuredToken = 0;
  {
    std::lock_guard lock(state->mutex);
    if (state->publishing) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          "MMS旧会话仍有在途DataCenter批次，暂不能重新启用");
    }
    state->statistics.set_mms_events_dropped(
        state->statistics.mms_events_dropped() + state->queuedValueCount);
    state->connId = config.connId;
    state->queueCapacity = config.queueCapacity;
    state->batchSize = config.batchSize;
    state->batchWindow = config.batchWindow;
    state->active = true;
    ++state->generation;
    ++state->activationToken;
    if (state->activationToken == 0) {
      ++state->activationToken;
    }
    configuredToken = state->activationToken;
    state->queuedValueCount = 0;
    state->queuedRetainedBytes = 0;
    state->queue.clear();
    state->mappings.clear();
    state->statistics.set_conn_name(config.connName);
    for (const auto& mapping : config.mappings.points()) {
      if (mapping.source() != IEC61850Proto::POINT_SOURCE_MMS) {
        continue;
      }
      auto& mappingState =
          state->mappings[MappingKey(mapping.data_ref(), mapping.fc())];
      mappingState.mapping = mapping;
    }
    mappingCount = state->mappings.size();
    EnsureWorkerLocked(config.connName, state);
    state->condition.notify_all();
  }
  if (activationToken != nullptr) {
    *activationToken = configuredToken;
  }
  LOG_INFO("IEC61850已配置MMS事件发布管线: IED={}, conn_id={}, 点数={}, 待处理点值容量={}, 批量点数={}, 窗口毫秒={}",
           config.connName, config.connId, mappingCount, config.queueCapacity,
           config.batchSize, config.batchWindow.count());
  return grpc::Status::OK;
}

std::function<void(MmsReportEvent)> MmsEventPipeline::MakeReportCallback(
    std::string_view connName, uint64_t activationToken) const {
  const auto state = FindState(connName);
  if (!state || activationToken == 0) {
    return {};
  }
  std::weak_ptr<IedPipelineState> weakState = state;
  return [weakState, connName = std::string(connName),
          activationToken](MmsReportEvent report) {
    if (const auto locked = weakState.lock()) {
      EnqueueReportForState(connName, locked, activationToken,
                            std::move(report));
    }
  };
}

void MmsEventPipeline::UpdateConnectionId(std::string_view connName,
                                          uint32_t connId) {
  const auto state = FindState(connName);
  if (!state) {
    return;
  }
  uint32_t previousConnId = 0;
  {
    std::lock_guard lock(state->mutex);
    if (state->connId == connId) {
      return;
    }
    previousConnId = state->connId;
    state->connId = connId;
    ++state->generation;
    for (auto& [_, mapping] : state->mappings) {
      mapping.published = {};
    }
    if (state->inFlightContext != nullptr) {
      state->inFlightContext->TryCancel();
    }
  }
  LOG_INFO("IEC61850 MMS发布连接标识已更新并清除死区基准: IED={}, 原conn_id={}, 新conn_id={}",
           connName, previousConnId, connId);
}

grpc::Status MmsEventPipeline::DeactivateIed(std::string_view connName) {
  auto status = InvalidateIed(connName);
  if (!status.ok()) {
    return status;
  }
  return WaitForDeactivation(connName);
}

grpc::Status MmsEventPipeline::InvalidateIed(std::string_view connName) {
  const auto state = FindState(connName);
  if (!state) {
    return grpc::Status::OK;
  }

  std::unique_lock lock(state->mutex);
  const auto dropped = state->queuedValueCount;
  const auto droppedBytes = state->queuedRetainedBytes;
  state->statistics.set_mms_events_dropped(
      state->statistics.mms_events_dropped() + dropped);
  state->queue.clear();
  state->queuedValueCount = 0;
  state->queuedRetainedBytes = 0;
  state->active = false;
  ++state->generation;
  ++state->activationToken;
  if (state->inFlightContext != nullptr) {
    state->inFlightContext->TryCancel();
  }
  state->condition.notify_all();
  LOG_INFO("IEC61850已立即使MMS报告入口失效: IED={}, 清理待发布点数={}, 清理估算字节数={}",
           connName, dropped, droppedBytes);
  return grpc::Status::OK;
}

grpc::Status MmsEventPipeline::WaitForDeactivation(
    std::string_view connName) {
  const auto state = FindState(connName);
  if (!state) {
    return grpc::Status::OK;
  }

  std::unique_lock lock(state->mutex);
  const bool quiesced = state->condition.wait_for(
      lock, kDeactivateWaitTimeout,
      [&state]() { return !state->publishing; });
  lock.unlock();

  if (!quiesced) {
    LOG_WARNING("IEC61850停止MMS发布入口时等待在途批次超时: IED={}",
                connName);
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "停止MMS发布入口时等待在途批次超时");
  }
  LOG_INFO("IEC61850已等待MMS报告入口收敛: IED={}", connName);
  return grpc::Status::OK;
}

void MmsEventPipeline::RemoveIed(std::string_view connName) {
  std::shared_ptr<IedPipelineState> state;
  {
    std::lock_guard lock(statesMutex_);
    const auto it = states_.find(std::string(connName));
    if (it == states_.end()) {
      return;
    }
    state = std::move(it->second);
    states_.erase(it);
  }
  StopWorker(state);
  LOG_INFO("IEC61850已释放MMS发布管线状态: IED={}", connName);
}

bool MmsEventPipeline::EnqueueReport(std::string_view connName,
                                     MmsReportEvent report) {
  const auto state = FindState(connName);
  if (!state) {
    return false;
  }
  uint64_t activationToken = 0;
  {
    std::lock_guard lock(state->mutex);
    activationToken = state->activationToken;
  }
  return EnqueueReportForState(std::string(connName), state,
                               activationToken, std::move(report));
}

bool MmsEventPipeline::EnqueueReportForState(
    const std::string& connName,
    const std::shared_ptr<IedPipelineState>& state,
    uint64_t activationToken, MmsReportEvent report) {
  const auto incomingValues = report.values.size();
  const auto oversizedValues = CountOversizedVariableValues(report);
  const auto retainedBytes = EstimateReportRetainedBytes(report);
  bool logOverflow = false;
  bool logOversizedReport = false;
  bool accepted = true;
  uint64_t droppedTotal = 0;
  uint64_t oversizedReportsTotal = 0;
  std::size_t queuedRetainedBytes = 0;
  {
    std::lock_guard lock(state->mutex);
    if (!state->active || activationToken == 0 ||
        state->activationToken != activationToken) {
      return false;
    }
    auto& statistics = state->statistics;
    const auto droppedBefore = statistics.mms_events_dropped();
    statistics.set_mms_reports_received(statistics.mms_reports_received() + 1);
    if (report.receiveTimestampMs <= 0) {
      report.receiveTimestampMs = CurrentTimestampMs();
    }
    statistics.set_last_event_ts_ms(
        static_cast<uint64_t>(std::max<int64_t>(report.receiveTimestampMs, 0)));
    if (oversizedValues > 0 ||
        retainedBytes > mskdsp::kIec61850MaxMmsReportRetainedBytes) {
      statistics.set_mms_values_oversized(
          statistics.mms_values_oversized() + oversizedValues);
      statistics.set_mms_reports_oversized(
          statistics.mms_reports_oversized() + 1);
      statistics.set_mms_events_dropped(statistics.mms_events_dropped() +
                                        incomingValues);
      droppedTotal = statistics.mms_events_dropped();
      oversizedReportsTotal = statistics.mms_reports_oversized();
      logOversizedReport = ShouldLogCounter(oversizedReportsTotal);
      accepted = false;
    } else if (report.values.empty()) {
      return true;
    } else if (incomingValues > state->queueCapacity ||
               retainedBytes >
                   mskdsp::kIec61850MaxMmsQueueRetainedBytes) {
      statistics.set_mms_events_dropped(statistics.mms_events_dropped() +
                                        incomingValues);
      droppedTotal = statistics.mms_events_dropped();
      logOverflow = ShouldLogDropTransition(droppedBefore, droppedTotal);
      accepted = false;
    } else {
      while (!state->queue.empty() &&
             (WouldExceed(state->queuedValueCount, incomingValues,
                          state->queueCapacity) ||
              WouldExceed(state->queuedRetainedBytes, retainedBytes,
                          mskdsp::kIec61850MaxMmsQueueRetainedBytes))) {
        const auto& oldest = state->queue.front();
        const auto dropped = oldest.report.values.size() - oldest.nextValue;
        state->queuedValueCount -= dropped;
        if (state->queuedRetainedBytes >= oldest.retainedBytes) {
          state->queuedRetainedBytes -= oldest.retainedBytes;
        } else {
          state->queuedRetainedBytes = 0;
        }
        statistics.set_mms_events_dropped(
            statistics.mms_events_dropped() + dropped);
        state->queue.pop_front();
      }
      droppedTotal = statistics.mms_events_dropped();
      logOverflow = ShouldLogDropTransition(droppedBefore, droppedTotal);
      state->queuedValueCount += incomingValues;
      state->queuedRetainedBytes += retainedBytes;
      state->queue.push_back(QueuedReport{.report = std::move(report),
                                          .retainedBytes = retainedBytes});
      statistics.set_mms_queue_high_watermark(std::max<uint64_t>(
          statistics.mms_queue_high_watermark(), state->queuedValueCount));
      statistics.set_mms_queue_bytes_high_watermark(std::max<uint64_t>(
          statistics.mms_queue_bytes_high_watermark(),
          state->queuedRetainedBytes));
      queuedRetainedBytes = state->queuedRetainedBytes;
      state->condition.notify_all();
    }
  }

  if (logOversizedReport) {
    LOG_WARNING("IEC61850 MMS报告超过字节安全边界: IED={}, 估算字节数={}, 超大成员数={}, 累计超大报告数={}, 累计丢弃点数={}",
                connName, retainedBytes, oversizedValues,
                oversizedReportsTotal, droppedTotal);
  }
  if (logOverflow) {
    LOG_WARNING("IEC61850 MMS待处理队列容量不足: IED={}, 当前估算字节数={}, 累计丢弃点数={}",
                connName, queuedRetainedBytes, droppedTotal);
  }
  return accepted;
}

IEC61850Proto::RuntimeStatistics MmsEventPipeline::GetStatistics(
    std::string_view connName) const {
  const auto state = FindState(connName);
  if (state) {
    std::lock_guard lock(state->mutex);
    return state->statistics;
  }
  IEC61850Proto::RuntimeStatistics statistics;
  statistics.set_conn_name(connName);
  return statistics;
}

bool MmsEventPipeline::WaitUntilIdle(
    std::chrono::milliseconds timeout) const {
  std::vector<std::shared_ptr<IedPipelineState>> states;
  {
    std::lock_guard lock(statesMutex_);
    states.reserve(states_.size());
    for (const auto& [_, state] : states_) {
      states.emplace_back(state);
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (const auto& state : states) {
    std::unique_lock lock(state->mutex);
    if (!state->condition.wait_until(lock, deadline, [&state]() {
          return !state->publishing && state->queue.empty();
        })) {
      return false;
    }
  }
  return true;
}

std::string MmsEventPipeline::MappingKey(
    std::string_view dataRef, IEC61850Proto::FunctionalConstraint fc) {
  return std::format("{}#{}", dataRef, static_cast<int>(fc));
}

int64_t MmsEventPipeline::CurrentTimestampMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

DataCenterProto::Quality MmsEventPipeline::ConvertQuality(
    const MmsQuality& quality, bool timestampValid) {
  if (quality.validity == MmsValidity::INVALID ||
      quality.validity == MmsValidity::RESERVED || quality.badReference ||
      quality.failure) {
    return DataCenterProto::QUALITY_BAD;
  }
  if (quality.validity == MmsValidity::QUESTIONABLE || !timestampValid ||
      quality.overflow || quality.outOfRange || quality.oscillatory ||
      quality.oldData || quality.inconsistent || quality.inaccurate ||
      quality.sourceSubstituted || quality.test || quality.operatorBlocked) {
    return DataCenterProto::QUALITY_UNCERTAIN;
  }
  return DataCenterProto::QUALITY_GOOD;
}

MmsEventPipeline::ConversionResult MmsEventPipeline::ConvertValue(
    const MmsDataValue& source, DataCenterProto::Quality quality,
    const MappingState& mapping, PublishedPointState* candidate,
    DataCenterProto::PointValue* value) {
  if (candidate == nullptr || value == nullptr) {
    return ConversionResult::INVALID_VALUE;
  }
  value->Clear();
  const auto& config = mapping.mapping;
  const auto acceptNumeric = [&](long double engineered) {
    if (!std::isfinite(engineered)) {
      return ConversionResult::INVALID_VALUE;
    }
    const bool sameQuality =
        !candidate->hasQuality || candidate->quality == quality;
    if (config.deadband() > 0.0 && candidate->hasNumericValue &&
        sameQuality &&
        std::abs(engineered - candidate->numericValue) <=
            static_cast<long double>(config.deadband())) {
      return ConversionResult::DEADBAND_FILTERED;
    }
    candidate->hasNumericValue = true;
    candidate->numericValue = engineered;
    candidate->hasQuality = true;
    candidate->quality = quality;
    return ConversionResult::PUBLISHED;
  };
  const auto acceptNonNumeric = [&]() {
    candidate->hasQuality = true;
    candidate->quality = quality;
    return ConversionResult::PUBLISHED;
  };

  switch (config.value_type()) {
  case IEC61850Proto::POINT_VALUE_TYPE_BOOL:
    if (const auto* raw = std::get_if<bool>(&source.value)) {
      value->set_bool_value(*raw);
      return acceptNonNumeric();
    }
    return ConversionResult::TYPE_MISMATCH;
  case IEC61850Proto::POINT_VALUE_TYPE_INT64: {
    const auto* integer = std::get_if<int64_t>(&source.value);
    const auto* number = std::get_if<double>(&source.value);
    if (integer == nullptr && number == nullptr) {
      return ConversionResult::TYPE_MISMATCH;
    }
    const long double scale =
        config.scale() == 0.0 ? 1.0L
                              : static_cast<long double>(config.scale());
    const long double offset = static_cast<long double>(config.offset());
    int64_t converted = 0;
    if (integer != nullptr && scale == 1.0L && offset == 0.0L) {
      converted = *integer;
    } else {
      const long double raw =
          integer != nullptr ? static_cast<long double>(*integer)
                             : static_cast<long double>(*number);
      const long double rounded = std::round(raw * scale + offset);
      constexpr long double kInt64Minimum = -9223372036854775808.0L;
      constexpr long double kInt64UpperExclusive = 9223372036854775808.0L;
      if (!std::isfinite(rounded) || rounded < kInt64Minimum ||
          rounded >= kInt64UpperExclusive) {
        return ConversionResult::INVALID_VALUE;
      }
      converted = static_cast<int64_t>(rounded);
    }
    const auto result = acceptNumeric(static_cast<long double>(converted));
    if (result == ConversionResult::PUBLISHED) {
      value->set_int_value(converted);
    }
    return result;
  }
  case IEC61850Proto::POINT_VALUE_TYPE_DOUBLE: {
    long double raw = 0.0L;
    if (const auto* number = std::get_if<double>(&source.value)) {
      raw = static_cast<long double>(*number);
    } else if (const auto* integer = std::get_if<int64_t>(&source.value)) {
      raw = static_cast<long double>(*integer);
    } else {
      return ConversionResult::TYPE_MISMATCH;
    }
    const long double scale =
        config.scale() == 0.0 ? 1.0L
                              : static_cast<long double>(config.scale());
    const long double engineered =
        raw * scale + static_cast<long double>(config.offset());
    const double converted = static_cast<double>(engineered);
    if (!std::isfinite(engineered) || !std::isfinite(converted)) {
      return ConversionResult::INVALID_VALUE;
    }
    const auto result = acceptNumeric(static_cast<long double>(converted));
    if (result == ConversionResult::PUBLISHED) {
      value->set_double_value(converted);
    }
    return result;
  }
  case IEC61850Proto::POINT_VALUE_TYPE_STRING:
    if (const auto* raw = std::get_if<std::string>(&source.value)) {
      value->set_string_value(*raw);
      return acceptNonNumeric();
    }
    return ConversionResult::TYPE_MISMATCH;
  case IEC61850Proto::POINT_VALUE_TYPE_BYTES:
    if (const auto* raw = std::get_if<std::vector<uint8_t>>(&source.value)) {
      value->set_bytes_value(raw->data(), raw->size());
      return acceptNonNumeric();
    }
    if (const auto* composite =
            std::get_if<std::shared_ptr<MmsCompositeValue>>(&source.value);
        composite != nullptr && *composite != nullptr &&
        !(*composite)->encodedContent.empty()) {
      value->set_bytes_value((*composite)->encodedContent.data(),
                             (*composite)->encodedContent.size());
      return acceptNonNumeric();
    }
    return ConversionResult::TYPE_MISMATCH;
  default:
    return ConversionResult::INVALID_VALUE;
  }
}

std::shared_ptr<MmsEventPipeline::IedPipelineState>
MmsEventPipeline::FindState(std::string_view connName) const {
  std::lock_guard lock(statesMutex_);
  const auto it = states_.find(std::string(connName));
  return it == states_.end() ? nullptr : it->second;
}

void MmsEventPipeline::EnsureWorkerLocked(
    const std::string& connName,
    const std::shared_ptr<IedPipelineState>& state) {
  if (state->worker.joinable()) {
    return;
  }
  state->worker = ModuleManager::StartModuleThread(
      "IEC61850", [this, connName, state](std::stop_token stopToken) {
        WorkerLoop(connName, state, stopToken);
      });
}

std::size_t MmsEventPipeline::QueuedValueCountLocked(
    const IedPipelineState& state) {
  return state.queuedValueCount;
}

void MmsEventPipeline::BuildBatchLocked(
    IedPipelineState* state, DataCenterProto::BatchPublishRequest* batch,
    BatchBuildState* buildState) {
  batch->Clear();
  buildState->generation = state->generation;
  buildState->candidates.clear();
  buildState->serializedBytes = 0;
  std::size_t processedValues = 0;
  bool batchFull = false;
  while (!state->queue.empty() && !batchFull &&
         processedValues < state->batchSize &&
         static_cast<std::size_t>(batch->points_size()) < state->batchSize) {
    auto& queued = state->queue.front();
    while (queued.nextValue < queued.report.values.size() &&
           !batchFull &&
           processedValues < state->batchSize &&
           static_cast<std::size_t>(batch->points_size()) <
               state->batchSize) {
      const auto& source = queued.report.values[queued.nextValue];
      const auto consumeValue = [&]() {
        ++queued.nextValue;
        ++processedValues;
        if (state->queuedValueCount > 0) {
          --state->queuedValueCount;
        }
      };
      const auto key = MappingKey(source.dataRef, source.fc);
      const auto mappingIt = state->mappings.find(key);
      if (mappingIt == state->mappings.end()) {
        consumeValue();
        auto& statistics = state->statistics;
        statistics.set_mms_values_unmapped(
            statistics.mms_values_unmapped() + 1);
        if (ShouldLogCounter(statistics.mms_values_unmapped())) {
          buildState->logUnmapped = true;
          buildState->firstUnmappedRef = source.dataRef;
        }
        continue;
      }
      PublishedPointState candidate = mappingIt->second.published;
      const bool timestampValid =
          source.timestampValid && source.timestampMs > 0;
      const auto quality = ConvertQuality(source.quality, timestampValid);
      DataCenterProto::PointValue converted;
      const auto result = ConvertValue(source, quality, mappingIt->second,
                                       &candidate, &converted);
      if (result == ConversionResult::DEADBAND_FILTERED) {
        consumeValue();
        state->statistics.set_mms_values_deadband_filtered(
            state->statistics.mms_values_deadband_filtered() + 1);
        continue;
      }
      if (result == ConversionResult::TYPE_MISMATCH) {
        consumeValue();
        auto& statistics = state->statistics;
        statistics.set_mms_values_type_mismatch(
            statistics.mms_values_type_mismatch() + 1);
        if (ShouldLogCounter(statistics.mms_values_type_mismatch())) {
          buildState->logTypeMismatch = true;
          buildState->firstTypeMismatchRef = source.dataRef;
        }
        continue;
      }
      if (result == ConversionResult::INVALID_VALUE) {
        consumeValue();
        auto& statistics = state->statistics;
        statistics.set_mms_values_invalid(
            statistics.mms_values_invalid() + 1);
        if (ShouldLogCounter(statistics.mms_values_invalid())) {
          buildState->logInvalid = true;
          buildState->firstInvalidRef = source.dataRef;
        }
        continue;
      }

      DataCenterProto::PublishRequest point;
      point.set_conn_id(state->connId);
      point.set_tag(mappingIt->second.mapping.tag());
      *point.mutable_value() = std::move(converted);
      point.set_ts_ms(timestampValid ? source.timestampMs
                                     : queued.report.receiveTimestampMs);
      point.set_quality(quality);
      const auto pointBytes = SerializedBatchPointBytes(point);
      if (pointBytes > mskdsp::kIec61850MaxMmsBatchSerializedBytes) {
        consumeValue();
        auto& statistics = state->statistics;
        statistics.set_mms_values_oversized(
            statistics.mms_values_oversized() + 1);
        statistics.set_mms_events_dropped(
            statistics.mms_events_dropped() + 1);
        buildState->logOversized = true;
        buildState->firstOversizedRef = source.dataRef;
        continue;
      }
      if (WouldExceed(buildState->serializedBytes, pointBytes,
                      mskdsp::kIec61850MaxMmsBatchSerializedBytes)) {
        batchFull = true;
        break;
      }
      consumeValue();
      *batch->add_points() = std::move(point);
      buildState->serializedBytes += pointBytes;
      auto candidateIt = buildState->candidates.find(key);
      if (candidateIt == buildState->candidates.end()) {
        buildState->candidates.emplace(key, candidate);
      } else {
        candidateIt->second = candidate;
      }
    }
    if (queued.nextValue == queued.report.values.size()) {
      if (state->queuedRetainedBytes >= queued.retainedBytes) {
        state->queuedRetainedBytes -= queued.retainedBytes;
      } else {
        state->queuedRetainedBytes = 0;
      }
      state->queue.pop_front();
    }
  }
}

void MmsEventPipeline::LogBatchDiagnostics(
    const std::string& connName,
    const BatchBuildState& buildState) noexcept {
  try {
    if (buildState.logUnmapped) {
      LOG_WARNING("IEC61850 MMS报告包含未映射点: IED={}, data_ref={}",
                  connName, buildState.firstUnmappedRef);
    }
    if (buildState.logTypeMismatch) {
      LOG_WARNING("IEC61850 MMS点值类型与映射不匹配: IED={}, data_ref={}",
                  connName, buildState.firstTypeMismatchRef);
    }
    if (buildState.logInvalid) {
      LOG_WARNING("IEC61850 MMS点值工程量转换无效: IED={}, data_ref={}",
                  connName, buildState.firstInvalidRef);
    }
    if (buildState.logOversized) {
      LOG_WARNING("IEC61850 MMS点值超过DataCenter批次字节边界: IED={}, data_ref={}",
                  connName, buildState.firstOversizedRef);
    }
  } catch (...) {
  }
}

void MmsEventPipeline::WorkerLoop(
    std::string connName, std::shared_ptr<IedPipelineState> state,
    std::stop_token stopToken) {
  LOG_INFO("IEC61850 MMS事件发布线程已启动: IED={}", connName);
  std::unique_lock lock(state->mutex);
  while (!stopToken.stop_requested()) {
    state->condition.wait(lock, stopToken,
                          [&state]() { return !state->queue.empty(); });
    if (stopToken.stop_requested()) {
      break;
    }
    if (state->batchWindow.count() > 0 &&
        QueuedValueCountLocked(*state) < state->batchSize) {
      const auto deadline =
          std::chrono::steady_clock::now() + state->batchWindow;
      state->condition.wait_until(lock, stopToken, deadline, [&state]() {
        return state->queue.empty() ||
               QueuedValueCountLocked(*state) >= state->batchSize;
      });
      if (stopToken.stop_requested()) {
        break;
      }
    }

    DataCenterProto::BatchPublishRequest batch;
    BatchBuildState buildState;
    const auto queuedBeforeBuild = state->queuedValueCount;
    bool buildFailed = false;
    try {
      BuildBatchLocked(state.get(), &batch, &buildState);
    } catch (...) {
      auto& statistics = state->statistics;
      statistics.set_mms_events_dropped(
          statistics.mms_events_dropped() + queuedBeforeBuild);
      statistics.set_data_center_publish_failures(
          statistics.data_center_publish_failures() + 1);
      state->queue.clear();
      state->queuedValueCount = 0;
      state->queuedRetainedBytes = 0;
      state->condition.notify_all();
      buildFailed = true;
    }
    if (buildFailed) {
      lock.unlock();
      try {
        LOG_ERROR("IEC61850 MMS构造DataCenter批次时发生异常: IED={}, 清理待发布点数={}",
                  connName, queuedBeforeBuild);
      } catch (...) {
      }
      lock.lock();
      continue;
    }
    if (batch.points().empty()) {
      state->condition.notify_all();
      lock.unlock();
      LogBatchDiagnostics(connName, buildState);
      lock.lock();
      continue;
    }

    std::unique_ptr<grpc::ClientContext> context;
    try {
      context = dataCenter_->CreateBatchPublishContext();
    } catch (...) {
    }
    if (!context) {
      auto& statistics = state->statistics;
      statistics.set_data_center_publish_failures(
          statistics.data_center_publish_failures() + 1);
      statistics.set_mms_events_dropped(
          statistics.mms_events_dropped() + batch.points_size());
      state->condition.notify_all();
      lock.unlock();
      try {
        LOG_ERROR("IEC61850创建DataCenter发布Context失败: IED={}, 丢弃批量点数={}",
                  connName, batch.points_size());
      } catch (...) {
      }
      lock.lock();
      continue;
    }
    state->publishing = true;
    state->inFlightContext = context.get();
    lock.unlock();
    grpc::Status status;
    bool publishReturned = false;
    try {
      LogBatchDiagnostics(connName, buildState);
      status = dataCenter_->BatchPublish(batch, context.get());
      publishReturned = true;
    } catch (...) {
    }
    lock.lock();
    state->inFlightContext = nullptr;
    state->publishing = false;
    state->condition.notify_all();

    auto& statistics = state->statistics;
    bool logPublishFailure = false;
    uint64_t publishFailureCount = 0;
    if (publishReturned && status.ok()) {
      if (state->generation == buildState.generation) {
        for (const auto& [key, candidate] : buildState.candidates) {
          const auto mappingIt = state->mappings.find(key);
          if (mappingIt != state->mappings.end()) {
            mappingIt->second.published = candidate;
          }
        }
      }
      statistics.set_data_center_batches_published(
          statistics.data_center_batches_published() + 1);
    } else {
      statistics.set_data_center_publish_failures(
          statistics.data_center_publish_failures() + 1);
      publishFailureCount = statistics.data_center_publish_failures();
      logPublishFailure = ShouldLogCounter(publishFailureCount);
    }
    lock.unlock();
    if (logPublishFailure) {
      try {
        if (publishReturned) {
          LOG_WARNING("IEC61850 MMS批量发布DataCenter失败: IED={}, 批量点数={}, 累计失败次数={}, 原因={}",
                      connName, batch.points_size(), publishFailureCount,
                      status.error_message());
        } else {
          LOG_WARNING("IEC61850 MMS批量发布DataCenter时适配层抛出异常: IED={}, 批量点数={}, 累计失败次数={}",
                      connName, batch.points_size(), publishFailureCount);
        }
      } catch (...) {
      }
    }
    lock.lock();
  }
  state->inFlightContext = nullptr;
  state->publishing = false;
  state->condition.notify_all();
  lock.unlock();
  LOG_INFO("IEC61850 MMS事件发布线程已停止: IED={}", connName);
}

void MmsEventPipeline::StopWorker(
    const std::shared_ptr<IedPipelineState>& state) {
  if (!state) {
    return;
  }
  {
    std::lock_guard lock(state->mutex);
    state->active = false;
    ++state->generation;
    ++state->activationToken;
    state->statistics.set_mms_events_dropped(
        state->statistics.mms_events_dropped() + state->queuedValueCount);
    state->queuedValueCount = 0;
    state->queuedRetainedBytes = 0;
    state->queue.clear();
    if (state->inFlightContext != nullptr) {
      state->inFlightContext->TryCancel();
    }
    if (state->worker.joinable()) {
      state->worker.request_stop();
    }
    state->condition.notify_all();
  }
  if (state->worker.joinable()) {
    state->worker.join();
  }
}

}  // namespace IEC61850
