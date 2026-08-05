#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "IEC61850DataCenterClient.h"
#include "IEC61850MmsPipeline.hpp"
#include "IEC61850ProtocolStack.h"
#include "mskdsp/IEC61850Limits.hpp"
#include "support/FakeDataCenter.hpp"

namespace {

constexpr auto kMmsMaxVariableValueBytes =
    mskdsp::kIec61850MaxMmsVariableValueBytes;
constexpr auto kMmsMaxReportRetainedBytes =
    mskdsp::kIec61850MaxMmsReportRetainedBytes;
constexpr auto kMmsMaxQueueRetainedBytes =
    mskdsp::kIec61850MaxMmsQueueRetainedBytes;
constexpr auto kMmsMaxBatchSerializedBytes =
    mskdsp::kIec61850MaxMmsBatchSerializedBytes;

IEC61850Proto::PointMappings MakeMappings(double deadband = 0.0) {
  IEC61850Proto::PointMappings mappings;
  mappings.set_conn_name("line-1");
  auto* point = mappings.add_points();
  point->set_tag("P");
  point->set_data_ref("IED1LD0/MMXU1.TotW.mag.f");
  point->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  point->set_source(IEC61850Proto::POINT_SOURCE_MMS);
  point->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_DOUBLE);
  point->set_scale(2.0);
  point->set_offset(1.0);
  point->set_deadband(deadband);
  return mappings;
}

IEC61850::MmsReportEvent MakeReport(double rawValue, int64_t timestampMs,
                                    bool timestampValid = true) {
  IEC61850::MmsReportEvent report;
  report.reportRef = "IED1LD0/LLN0$BR$measurements";
  report.dataSetRef = "IED1LD0/LLN0$measurements";
  report.confRev = 1;
  report.sequenceNumber = 7;
  report.receiveTimestampMs = 5000;
  auto& value = report.values.emplace_back();
  value.dataRef = "IED1LD0/MMXU1.TotW.mag.f";
  value.fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX;
  value.value = rawValue;
  value.timestampMs = timestampMs;
  value.timestampValid = timestampValid;
  value.quality.validity = IEC61850::MmsValidity::GOOD;
  return report;
}

IEC61850::MmsReportEvent MakeVariableReport(
    IEC61850::MmsValue value, int64_t timestampMs = 1000) {
  auto report = MakeReport(0.0, timestampMs);
  report.values.front().value = std::move(value);
  return report;
}

IEC61850::MmsPublishConfig MakePublishConfig(
    const IEC61850Proto::PointMappings& mappings) {
  IEC61850::MmsPublishConfig config;
  config.connName = "line-1";
  config.connId = 11;
  config.mappings = mappings;
  config.queueCapacity = 8;
  config.batchSize = 4;
  config.batchWindow = std::chrono::milliseconds(1);
  return config;
}

// 验证：MMS报告按data_ref+fc映射，执行工程量换算并通过BatchPublish进入DataCenter。
TEST(IEC61850MmsPipelineTest, MapsAndPublishesMmsReport) {
  FakeDataCenterState state;
  state.AddConnection(11, "IEC61850", "line-1");
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings())).ok());

  EXPECT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(10.0, 1234)));

  ASSERT_TRUE(state.WaitForPublishCount(
      11, "P", 1, std::chrono::seconds(2)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));
  DataCenterProto::GetLatestRequest latestRequest;
  latestRequest.set_conn_id(11);
  latestRequest.add_tags("P");
  DataCenterProto::GetLatestResponse latest;
  ASSERT_TRUE(state.GetLatest(latestRequest, &latest).ok());
  ASSERT_EQ(latest.updates_size(), 1);
  EXPECT_DOUBLE_EQ(latest.updates(0).value().double_value(), 21.0);
  EXPECT_EQ(latest.updates(0).ts_ms(), 1234);
  EXPECT_EQ(latest.updates(0).quality(), DataCenterProto::QUALITY_GOOD);
  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_EQ(statistics.mms_reports_received(), 1u);
  EXPECT_EQ(statistics.data_center_batches_published(), 1u);
  EXPECT_EQ(statistics.data_center_publish_failures(), 0u);
}

// 验证：MMS的STRING和BYTES点值保持原始内容并发布到DataCenter。
TEST(IEC61850MmsPipelineTest, PublishesStringAndBytesValues) {
  FakeDataCenterState state;
  state.AddConnection(11, "IEC61850", "line-1");
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  auto mappings = MakeMappings();
  auto* stringPoint = mappings.mutable_points(0);
  stringPoint->set_tag("S");
  stringPoint->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_STRING);
  auto* bytesPoint = mappings.add_points();
  *bytesPoint = *stringPoint;
  bytesPoint->set_tag("B");
  bytesPoint->set_data_ref("IED1LD0/MMXU1.vendorBlob");
  bytesPoint->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BYTES);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(mappings)).ok());

  auto report = MakeVariableReport(
      std::string(kMmsMaxVariableValueBytes, 'x'));
  auto bytesValue = report.values.front();
  bytesValue.dataRef = bytesPoint->data_ref();
  bytesValue.value = std::vector<uint8_t>{0x00, 0x7f, 0xff};
  report.values.push_back(std::move(bytesValue));
  ASSERT_TRUE(pipeline.EnqueueReport("line-1", std::move(report)));

  ASSERT_TRUE(state.WaitForPublishCount(
      11, "S", 1, std::chrono::seconds(2)));
  ASSERT_TRUE(state.WaitForPublishCount(
      11, "B", 1, std::chrono::seconds(2)));
  DataCenterProto::GetLatestRequest latestRequest;
  latestRequest.set_conn_id(11);
  latestRequest.add_tags("S");
  latestRequest.add_tags("B");
  DataCenterProto::GetLatestResponse latest;
  ASSERT_TRUE(state.GetLatest(latestRequest, &latest).ok());
  ASSERT_EQ(latest.updates_size(), 2);
  EXPECT_EQ(latest.updates(0).value().string_value().size(),
            kMmsMaxVariableValueBytes);
  EXPECT_EQ(latest.updates(0).value().string_value().front(), 'x');
  EXPECT_EQ(latest.updates(1).value().bytes_value(),
            std::string("\x00\x7f\xff", 3));
}

// 验证：任一STRING/BYTES成员超过单值上限时整份MMS报告被拒绝并统计。
TEST(IEC61850MmsPipelineTest, RejectsReportContainingOversizedVariableValue) {
  FakeDataCenterState state;
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  auto mappings = MakeMappings();
  mappings.mutable_points(0)->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_STRING);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(mappings)).ok());

  auto report = MakeVariableReport(
      std::string(kMmsMaxVariableValueBytes + 1, 'x'));
  EXPECT_FALSE(pipeline.EnqueueReport("line-1", std::move(report)));

  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_EQ(statistics.mms_reports_received(), 1u);
  EXPECT_EQ(statistics.mms_reports_oversized(), 1u);
  EXPECT_EQ(statistics.mms_values_oversized(), 1u);
  EXPECT_EQ(statistics.mms_events_dropped(), 1u);
}

// 验证：每个成员都合法但累计保留内存超过上限时整份MMS报告被拒绝。
TEST(IEC61850MmsPipelineTest, RejectsReportAboveRetainedByteLimit) {
  FakeDataCenterState state;
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  auto mappings = MakeMappings();
  mappings.mutable_points(0)->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_STRING);
  auto config = MakePublishConfig(mappings);
  config.queueCapacity = 32;
  ASSERT_TRUE(pipeline.ConfigureIed(std::move(config)).ok());

  auto report = MakeVariableReport(
      std::string(kMmsMaxVariableValueBytes, 'x'));
  while (report.values.size() * kMmsMaxVariableValueBytes <=
         kMmsMaxReportRetainedBytes) {
    report.values.push_back(report.values.front());
  }
  const auto valueCount = report.values.size();
  EXPECT_FALSE(pipeline.EnqueueReport("line-1", std::move(report)));

  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_EQ(statistics.mms_reports_oversized(), 1u);
  EXPECT_EQ(statistics.mms_values_oversized(), 0u);
  EXPECT_EQ(statistics.mms_events_dropped(), valueCount);
}

// 验证：空报告的逻辑保留内存恰好等于上限时接受，多一个字节时拒绝。
TEST(IEC61850MmsPipelineTest, EnforcesRetainedByteLimitForEmptyReport) {
  FakeDataCenterState state;
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings())).ok());

  IEC61850::MmsReportEvent exact;
  exact.reportRef.assign(kMmsMaxReportRetainedBytes - 128, 'r');
  EXPECT_TRUE(pipeline.EnqueueReport("line-1", std::move(exact)));
  IEC61850::MmsReportEvent oversized;
  oversized.reportRef.assign(kMmsMaxReportRetainedBytes - 127, 'r');
  EXPECT_FALSE(pipeline.EnqueueReport("line-1", std::move(oversized)));

  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_EQ(statistics.mms_reports_received(), 2u);
  EXPECT_EQ(statistics.mms_reports_oversized(), 1u);
  EXPECT_EQ(statistics.mms_values_oversized(), 0u);
}

// 验证：点数容量仍有余量时，每IED队列也会按固定字节预算淘汰较旧报告。
TEST(IEC61850MmsPipelineTest, QueueEvictsOldReportAtRetainedByteLimit) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  std::mutex mutex;
  std::condition_variable condition;
  bool firstBatchEntered = false;
  bool releaseFirstBatch = false;
  EXPECT_CALL(*stub, BatchPublish(testing::_, testing::_, testing::_))
      .WillRepeatedly(testing::Invoke(
          [&](grpc::ClientContext*,
              const DataCenterProto::BatchPublishRequest&,
              DataCenterProto::Empty*) {
            std::unique_lock lock(mutex);
            if (!firstBatchEntered) {
              firstBatchEntered = true;
              condition.notify_all();
              condition.wait(lock, [&]() { return releaseFirstBatch; });
            }
            return grpc::Status::OK;
          }));
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(stub);
  IEC61850::MmsEventPipeline pipeline(&client);
  auto mappings = MakeMappings();
  mappings.mutable_points(0)->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_STRING);
  auto config = MakePublishConfig(mappings);
  config.queueCapacity = 100;
  config.batchSize = 1;
  ASSERT_TRUE(pipeline.ConfigureIed(std::move(config)).ok());

  const auto makeLargeReport = []() {
    auto report = MakeVariableReport(
        std::string(kMmsMaxVariableValueBytes, 'x'));
    for (int index = 1; index < 15; ++index) {
      report.values.push_back(report.values.front());
    }
    return report;
  };
  ASSERT_TRUE(pipeline.EnqueueReport("line-1", makeLargeReport()));
  bool entered = false;
  {
    std::unique_lock lock(mutex);
    entered = condition.wait_for(
        lock, std::chrono::seconds(2), [&]() { return firstBatchEntered; });
    if (!entered) {
      releaseFirstBatch = true;
      condition.notify_all();
    }
  }
  ASSERT_TRUE(entered);
  for (int index = 0; index < 4; ++index) {
    EXPECT_TRUE(pipeline.EnqueueReport("line-1", makeLargeReport()));
  }

  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_GT(statistics.mms_events_dropped(), 0u);
  EXPECT_LE(statistics.mms_queue_bytes_high_watermark(),
            kMmsMaxQueueRetainedBytes);
  {
    std::lock_guard lock(mutex);
    releaseFirstBatch = true;
    condition.notify_all();
  }
  pipeline.DeactivateIed("line-1");
}

// 验证：DataCenter批次按序列化字节上限拆分，且每个批次均不超过安全边界。
TEST(IEC61850MmsPipelineTest, SplitsPublishBatchAtSerializedByteLimit) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  std::mutex mutex;
  std::vector<std::size_t> batchBytes;
  std::size_t publishedPoints = 0;
  EXPECT_CALL(*stub, BatchPublish(testing::_, testing::_, testing::_))
      .WillRepeatedly(testing::Invoke(
          [&](grpc::ClientContext*,
              const DataCenterProto::BatchPublishRequest& request,
              DataCenterProto::Empty*) {
            std::lock_guard lock(mutex);
            batchBytes.push_back(request.ByteSizeLong());
            publishedPoints += static_cast<std::size_t>(request.points_size());
            return grpc::Status::OK;
          }));
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(stub);
  IEC61850::MmsEventPipeline pipeline(&client);
  auto mappings = MakeMappings();
  mappings.mutable_points(0)->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_STRING);
  auto config = MakePublishConfig(mappings);
  config.queueCapacity = 32;
  config.batchSize = 32;
  ASSERT_TRUE(pipeline.ConfigureIed(std::move(config)).ok());

  auto report = MakeVariableReport(
      std::string(kMmsMaxVariableValueBytes - 1024, 'x'));
  for (int index = 1; index < 13; ++index) {
    report.values.push_back(report.values.front());
  }
  ASSERT_TRUE(pipeline.EnqueueReport("line-1", std::move(report)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));

  std::lock_guard lock(mutex);
  EXPECT_EQ(publishedPoints, 13u);
  ASSERT_GE(batchBytes.size(), 2u);
  for (const auto bytes : batchBytes) {
    EXPECT_LE(bytes, kMmsMaxBatchSerializedBytes);
  }
}

// 验证：映射后的单点无法放入空批次时只丢该点，不发送空DataCenter请求。
TEST(IEC61850MmsPipelineTest, DropsPointThatCannotFitEmptyBatch) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  EXPECT_CALL(*stub, BatchPublish(testing::_, testing::_, testing::_)).Times(0);
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(stub);
  IEC61850::MmsEventPipeline pipeline(&client);
  auto mappings = MakeMappings();
  mappings.mutable_points(0)->set_tag(
      std::string(kMmsMaxBatchSerializedBytes + 1, 'T'));
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(mappings)).ok());

  ASSERT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(1.0, 1000)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));

  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_EQ(statistics.mms_values_oversized(), 1u);
  EXPECT_EQ(statistics.mms_events_dropped(), 1u);
  EXPECT_EQ(statistics.data_center_batches_published(), 0u);
}

// 验证：MMS发布管线自身也拒绝超过安全上限的运行参数。
TEST(IEC61850MmsPipelineTest, RejectsPublishConfigurationAboveLimits) {
  FakeDataCenterState state;
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);

  auto config = MakePublishConfig(MakeMappings());
  config.queueCapacity = 65537;
  EXPECT_EQ(pipeline.ConfigureIed(config).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  config = MakePublishConfig(MakeMappings());
  config.batchSize = 4097;
  EXPECT_EQ(pipeline.ConfigureIed(config).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  config = MakePublishConfig(MakeMappings());
  config.batchWindow = std::chrono::milliseconds(1001);
  EXPECT_EQ(pipeline.ConfigureIed(config).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：无效IEC61850时标回退到接收时刻，且品质不会高于UNCERTAIN。
TEST(IEC61850MmsPipelineTest, InvalidTimestampUsesReceiveTimeAndDegradesQuality) {
  FakeDataCenterState state;
  state.AddConnection(11, "IEC61850", "line-1");
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings())).ok());

  EXPECT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(2.0, 0, false)));

  ASSERT_TRUE(state.WaitForPublishCount(
      11, "P", 1, std::chrono::seconds(2)));
  DataCenterProto::GetLatestRequest latestRequest;
  latestRequest.set_conn_id(11);
  latestRequest.add_tags("P");
  DataCenterProto::GetLatestResponse latest;
  ASSERT_TRUE(state.GetLatest(latestRequest, &latest).ok());
  ASSERT_EQ(latest.updates_size(), 1);
  EXPECT_EQ(latest.updates(0).ts_ms(), 5000);
  EXPECT_EQ(latest.updates(0).quality(), DataCenterProto::QUALITY_UNCERTAIN);
}

// 验证：正死区会过滤未越过阈值的工程量变化。
TEST(IEC61850MmsPipelineTest, DeadbandFiltersSmallEngineeringChange) {
  FakeDataCenterState state;
  state.AddConnection(11, "IEC61850", "line-1");
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings(1.0))).ok());

  EXPECT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(10.0, 1000)));
  ASSERT_TRUE(state.WaitForPublishCount(
      11, "P", 1, std::chrono::seconds(2)));
  EXPECT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(10.2, 1001)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));

  EXPECT_EQ(state.GetPublishCount(11, "P"), 1u);
  EXPECT_EQ(pipeline.GetStatistics("line-1").mms_values_deadband_filtered(),
            1u);
}

// 验证：数值未变但品质变差时必须绕过死区并发布新品质。
TEST(IEC61850MmsPipelineTest, QualityChangeBypassesNumericDeadband) {
  FakeDataCenterState state;
  state.AddConnection(11, "IEC61850", "line-1");
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings(1.0))).ok());

  EXPECT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(10.0, 1000)));
  ASSERT_TRUE(state.WaitForPublishCount(
      11, "P", 1, std::chrono::seconds(2)));
  auto degraded = MakeReport(10.0, 1001);
  degraded.values.front().quality.validity = IEC61850::MmsValidity::INVALID;
  EXPECT_TRUE(pipeline.EnqueueReport("line-1", std::move(degraded)));

  ASSERT_TRUE(state.WaitForPublishCount(
      11, "P", 2, std::chrono::seconds(2)));
  DataCenterProto::GetLatestRequest latestRequest;
  latestRequest.set_conn_id(11);
  latestRequest.add_tags("P");
  DataCenterProto::GetLatestResponse latest;
  ASSERT_TRUE(state.GetLatest(latestRequest, &latest).ok());
  ASSERT_EQ(latest.updates_size(), 1);
  EXPECT_EQ(latest.updates(0).quality(), DataCenterProto::QUALITY_BAD);
}

// 验证：DataCenter发布失败不推进死区基准，恢复后相同值仍会重新发布。
TEST(IEC61850MmsPipelineTest, FailedPublishDoesNotAdvanceDeadbandBaseline) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  EXPECT_CALL(*stub, BatchPublish(testing::_, testing::_, testing::_))
      .WillOnce(testing::Return(grpc::Status(
          grpc::StatusCode::UNAVAILABLE, "DataCenter暂不可用")))
      .WillOnce(testing::Return(grpc::Status::OK));
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(stub);
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings(1.0))).ok());

  EXPECT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(10.0, 1000)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));
  EXPECT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(10.0, 1001)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));

  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_EQ(statistics.data_center_publish_failures(), 1u);
  EXPECT_EQ(statistics.data_center_batches_published(), 1u);
}

// 验证：同一失败批次内的多个值都只相对最近成功基准过滤，不使用未提交候选值。
TEST(IEC61850MmsPipelineTest, FailedBatchDoesNotUseTemporaryDeadbandBaseline) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  std::size_t failedBatchPointCount = 0;
  EXPECT_CALL(*stub, BatchPublish(testing::_, testing::_, testing::_))
      .Times(3)
      .WillOnce(testing::Return(grpc::Status::OK))
      .WillOnce(testing::Invoke(
          [&](grpc::ClientContext*,
              const DataCenterProto::BatchPublishRequest& request,
              DataCenterProto::Empty*) {
            failedBatchPointCount =
                static_cast<std::size_t>(request.points_size());
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "模拟DataCenter失败");
          }))
      .WillOnce(testing::Return(grpc::Status::OK));
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(stub);
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(
      pipeline.ConfigureIed(MakePublishConfig(MakeMappings(1.0))).ok());

  ASSERT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(10.0, 1000)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));
  auto failedReport = MakeReport(11.0, 1001);
  auto secondValue = failedReport.values.front();
  secondValue.value = 10.6;
  secondValue.timestampMs = 1002;
  failedReport.values.push_back(std::move(secondValue));
  ASSERT_TRUE(pipeline.EnqueueReport("line-1", std::move(failedReport)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));

  EXPECT_EQ(failedBatchPointCount, 2u);
  ASSERT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(10.6, 1003)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));
  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_EQ(statistics.data_center_publish_failures(), 1u);
  EXPECT_EQ(statistics.data_center_batches_published(), 2u);
}

// 验证：DataCenter适配层抛异常后清理在途状态，同一IED工作线程仍能发布后续报告。
TEST(IEC61850MmsPipelineTest, ContinuesAfterPublishAdapterThrows) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  EXPECT_CALL(*stub, BatchPublish(testing::_, testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [](grpc::ClientContext*,
             const DataCenterProto::BatchPublishRequest&,
             DataCenterProto::Empty*) -> grpc::Status {
            throw std::runtime_error("DataCenter适配层异常");
          }))
      .WillOnce(testing::Return(grpc::Status::OK));
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(stub);
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings())).ok());

  ASSERT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(1.0, 1000)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));
  ASSERT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(2.0, 1001)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));

  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_EQ(statistics.data_center_publish_failures(), 1u);
  EXPECT_EQ(statistics.data_center_batches_published(), 1u);
}

// 验证：DataCenter连接标识变化后清除旧死区基准，相同值会向新连接发布首值。
TEST(IEC61850MmsPipelineTest, ConnectionIdChangeResetsDeadbandBaseline) {
  FakeDataCenterState state;
  state.AddConnection(11, "IEC61850", "line-1-old");
  state.AddConnection(12, "IEC61850", "line-1");
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings(1.0))).ok());
  ASSERT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(10.0, 1000)));
  ASSERT_TRUE(state.WaitForPublishCount(
      11, "P", 1, std::chrono::seconds(2)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));

  pipeline.UpdateConnectionId("line-1", 12);
  ASSERT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(10.0, 1001)));

  EXPECT_TRUE(state.WaitForPublishCount(
      12, "P", 1, std::chrono::seconds(2)));
}

// 验证：未映射、类型不匹配和无效工程量不发布且分项记录计数。
TEST(IEC61850MmsPipelineTest, CountsUnmappedTypeMismatchAndInvalidValues) {
  FakeDataCenterState state;
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings())).ok());
  auto report = MakeReport(1.0, 1000);
  report.values.front().value = std::string("wrong-type");
  auto missing = report.values.front();
  missing.dataRef = "IED1LD0/MMXU1.Missing.mag.f";
  missing.value = 2.0;
  report.values.push_back(std::move(missing));
  auto invalid = report.values.front();
  invalid.value = std::numeric_limits<double>::infinity();
  report.values.push_back(std::move(invalid));

  EXPECT_TRUE(pipeline.EnqueueReport("line-1", std::move(report)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));

  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_EQ(statistics.mms_values_unmapped(), 1u);
  EXPECT_EQ(statistics.mms_values_type_mismatch(), 1u);
  EXPECT_EQ(statistics.mms_values_invalid(), 1u);
  EXPECT_EQ(state.GetPublishCount(11, "P"), 0u);
}

// 验证：单份报告点数超过IED队列容量时整份拒绝并记录丢弃点数。
TEST(IEC61850MmsPipelineTest, RejectsReportLargerThanPointCapacity) {
  FakeDataCenterState state;
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  auto config = MakePublishConfig(MakeMappings());
  config.queueCapacity = 2;
  ASSERT_TRUE(pipeline.ConfigureIed(std::move(config)).ok());
  auto report = MakeReport(1.0, 1000);
  report.values.push_back(report.values.front());
  report.values.push_back(report.values.front());

  EXPECT_FALSE(pipeline.EnqueueReport("line-1", std::move(report)));

  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_EQ(statistics.mms_events_dropped(), 3u);
  EXPECT_EQ(statistics.mms_queue_high_watermark(), 0u);
}

// 验证：超过单批上限的大报告会被分成多个DataCenter批次而不丢点。
TEST(IEC61850MmsPipelineTest, SplitsLargeReportAcrossPublishBatches) {
  FakeDataCenterState state;
  state.AddConnection(11, "IEC61850", "line-1");
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  auto config = MakePublishConfig(MakeMappings());
  config.queueCapacity = 16;
  config.batchSize = 3;
  ASSERT_TRUE(pipeline.ConfigureIed(std::move(config)).ok());
  auto report = MakeReport(0.0, 1000);
  for (int index = 1; index < 10; ++index) {
    auto value = report.values.front();
    value.value = static_cast<double>(index);
    report.values.push_back(std::move(value));
  }

  EXPECT_TRUE(pipeline.EnqueueReport("line-1", std::move(report)));

  ASSERT_TRUE(state.WaitForPublishCount(
      11, "P", 10, std::chrono::seconds(2)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));
  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_EQ(statistics.data_center_batches_published(), 4u);
  EXPECT_EQ(statistics.mms_events_dropped(), 0u);
  EXPECT_EQ(statistics.mms_queue_high_watermark(), 10u);
}

// 验证：一个IED的DataCenter发布阻塞时另一个IED仍能独立完成发布。
TEST(IEC61850MmsPipelineTest, BlockedIedDoesNotBlockAnotherIed) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  std::mutex mutex;
  std::condition_variable condition;
  bool firstIedEntered = false;
  bool releaseFirstIed = false;
  bool secondIedCompleted = false;
  EXPECT_CALL(*stub, BatchPublish(testing::_, testing::_, testing::_))
      .Times(2)
      .WillRepeatedly(testing::Invoke(
          [&](grpc::ClientContext*,
              const DataCenterProto::BatchPublishRequest& request,
              DataCenterProto::Empty*) {
            if (!request.points().empty() &&
                request.points(0).conn_id() == 11) {
              std::unique_lock lock(mutex);
              firstIedEntered = true;
              condition.notify_all();
              condition.wait(lock, [&]() { return releaseFirstIed; });
            } else {
              std::lock_guard lock(mutex);
              secondIedCompleted = true;
              condition.notify_all();
            }
            return grpc::Status::OK;
          }));
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(stub);
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings())).ok());
  auto secondMappings = MakeMappings();
  secondMappings.set_conn_name("line-2");
  secondMappings.mutable_points(0)->set_tag("Q");
  auto secondConfig = MakePublishConfig(secondMappings);
  secondConfig.connName = "line-2";
  secondConfig.connId = 12;
  ASSERT_TRUE(pipeline.ConfigureIed(std::move(secondConfig)).ok());
  ASSERT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(1.0, 1000)));
  bool firstEntered = false;
  {
    std::unique_lock lock(mutex);
    firstEntered = condition.wait_for(
        lock, std::chrono::seconds(2), [&]() { return firstIedEntered; });
    if (!firstEntered) {
      releaseFirstIed = true;
      condition.notify_all();
    }
  }
  ASSERT_TRUE(firstEntered);

  const bool secondEnqueued =
      pipeline.EnqueueReport("line-2", MakeReport(2.0, 1001));
  if (!secondEnqueued) {
    std::lock_guard lock(mutex);
    releaseFirstIed = true;
    condition.notify_all();
  }
  ASSERT_TRUE(secondEnqueued);
  bool completedWithoutRelease = false;
  {
    std::unique_lock lock(mutex);
    completedWithoutRelease = condition.wait_for(
        lock, std::chrono::seconds(2), [&]() { return secondIedCompleted; });
    releaseFirstIed = true;
    condition.notify_all();
  }

  EXPECT_TRUE(completedWithoutRelease);
  EXPECT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));
}

// 验证：INT64原始值在边界和2的53次方以上不经double丢失精度。
TEST(IEC61850MmsPipelineTest, PreservesExactInt64Values) {
  FakeDataCenterState state;
  state.AddConnection(11, "IEC61850", "line-1");
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  auto mappings = MakeMappings();
  auto* point = mappings.mutable_points(0);
  point->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_INT64);
  point->set_scale(1.0);
  point->set_offset(0.0);
  point->set_deadband(0.0);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(mappings)).ok());
  const auto assertPublished = [&](int64_t raw, std::size_t expectedCount) {
    auto report = MakeReport(0.0, 1000 + expectedCount);
    report.values.front().value = raw;
    EXPECT_TRUE(pipeline.EnqueueReport("line-1", std::move(report)));
    EXPECT_TRUE(state.WaitForPublishCount(
        11, "P", expectedCount, std::chrono::seconds(2)));
    DataCenterProto::GetLatestRequest request;
    request.set_conn_id(11);
    request.add_tags("P");
    DataCenterProto::GetLatestResponse latest;
    EXPECT_TRUE(state.GetLatest(request, &latest).ok());
    ASSERT_EQ(latest.updates_size(), 1);
    EXPECT_EQ(latest.updates(0).value().int_value(), raw);
  };

  assertPublished(std::numeric_limits<int64_t>::max(), 1);
  assertPublished(std::numeric_limits<int64_t>::min(), 2);
  assertPublished(INT64_C(9007199254740993), 3);
}

// 验证：真正删除IED会释放管线状态，同名IED重建后统计从零开始。
TEST(IEC61850MmsPipelineTest, RemoveIedClearsPipelineState) {
  FakeDataCenterState state;
  state.AddConnection(11, "IEC61850", "line-1");
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings())).ok());
  ASSERT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(1.0, 1000)));
  ASSERT_TRUE(state.WaitForPublishCount(
      11, "P", 1, std::chrono::seconds(2)));
  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));

  pipeline.RemoveIed("line-1");

  EXPECT_EQ(pipeline.GetStatistics("line-1").mms_reports_received(), 0u);
  EXPECT_FALSE(pipeline.EnqueueReport("line-1", MakeReport(2.0, 1001)));
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings())).ok());
  EXPECT_EQ(pipeline.GetStatistics("line-1").mms_reports_received(), 0u);
}

// 验证：停止IED通信功能后，迟到的协议栈报告不会再进入发布队列。
TEST(IEC61850MmsPipelineTest, DeactivatedIedRejectsLateReport) {
  FakeDataCenterState state;
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings())).ok());

  pipeline.DeactivateIed("line-1");

  EXPECT_FALSE(pipeline.EnqueueReport("line-1", MakeReport(1.0, 1000)));
}

// 验证：批量关闭前先立即使指定IED入口失效，不等待在途发布即可拒绝迟到报告。
TEST(IEC61850MmsPipelineTest, InvalidatedIedRejectsIngressBeforeWait) {
  FakeDataCenterState state;
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(MakeStub(&state));
  IEC61850::MmsEventPipeline pipeline(&client);
  auto lineOne = MakePublishConfig(MakeMappings());
  lineOne.connName = "line-1";
  auto lineTwo = MakePublishConfig(MakeMappings());
  lineTwo.connName = "line-2";
  lineTwo.mappings.set_conn_name("line-2");
  ASSERT_TRUE(pipeline.ConfigureIed(std::move(lineOne)).ok());
  ASSERT_TRUE(pipeline.ConfigureIed(std::move(lineTwo)).ok());

  ASSERT_TRUE(pipeline.InvalidateIed("line-1").ok());

  EXPECT_FALSE(pipeline.EnqueueReport("line-1", MakeReport(1.0, 1000)));
  EXPECT_TRUE(pipeline.EnqueueReport("line-2", MakeReport(2.0, 1001)));
  ASSERT_TRUE(pipeline.WaitForDeactivation("line-1").ok());
}

// 验证：在途DataCenter发布未响应取消时，停止MMS入口返回超时而不假报成功。
TEST(IEC61850MmsPipelineTest, DeactivateReportsInFlightPublishTimeout) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  EXPECT_CALL(*stub, BatchPublish(testing::_, testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](grpc::ClientContext*,
              const DataCenterProto::BatchPublishRequest&,
              DataCenterProto::Empty*) {
            std::unique_lock lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&]() { return release; });
            return grpc::Status::OK;
          }));
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(stub);
  IEC61850::MmsEventPipeline pipeline(&client);
  ASSERT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings())).ok());
  ASSERT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(1.0, 1000)));
  bool publishEntered = false;
  {
    std::unique_lock lock(mutex);
    publishEntered = condition.wait_for(
        lock, std::chrono::seconds(2), [&]() { return entered; });
    if (!publishEntered) {
      release = true;
      condition.notify_all();
    }
  }
  ASSERT_TRUE(publishEntered);

  const auto status = pipeline.DeactivateIed("line-1");
  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_EQ(
      pipeline.ConfigureIed(MakePublishConfig(MakeMappings())).error_code(),
      grpc::StatusCode::FAILED_PRECONDITION);
  {
    std::lock_guard lock(mutex);
    release = true;
    condition.notify_all();
  }
  EXPECT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));
  EXPECT_TRUE(pipeline.ConfigureIed(MakePublishConfig(MakeMappings())).ok());
}

// 验证：DataCenter发布阻塞时容量为1的队列淘汰旧报告而不阻塞新报告回调。
TEST(IEC61850MmsPipelineTest, FullQueueKeepsNewReportAndCountsDroppedPoints) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  std::mutex mutex;
  std::condition_variable condition;
  bool firstBatchEntered = false;
  bool releaseFirstBatch = false;
  EXPECT_CALL(*stub, BatchPublish(testing::_, testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](grpc::ClientContext*,
              const DataCenterProto::BatchPublishRequest&,
              DataCenterProto::Empty*) {
            std::unique_lock lock(mutex);
            firstBatchEntered = true;
            condition.notify_all();
            condition.wait(lock, [&]() { return releaseFirstBatch; });
            return grpc::Status::OK;
          }))
      .WillOnce(testing::Return(grpc::Status::OK));
  IEC61850::DataCenterClient client("IEC61850");
  client.SetStub(stub);
  IEC61850::MmsEventPipeline pipeline(&client);
  auto config = MakePublishConfig(MakeMappings());
  config.queueCapacity = 1;
  config.batchSize = 1;
  ASSERT_TRUE(pipeline.ConfigureIed(std::move(config)).ok());
  ASSERT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(1.0, 1000)));
  {
    std::unique_lock lock(mutex);
    const bool entered = condition.wait_for(
        lock, std::chrono::seconds(2), [&]() { return firstBatchEntered; });
    if (!entered) {
      releaseFirstBatch = true;
      condition.notify_all();
    }
    ASSERT_TRUE(entered);
  }

  EXPECT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(2.0, 1001)));
  EXPECT_TRUE(pipeline.EnqueueReport("line-1", MakeReport(3.0, 1002)));
  {
    std::lock_guard lock(mutex);
    releaseFirstBatch = true;
    condition.notify_all();
  }

  ASSERT_TRUE(pipeline.WaitUntilIdle(std::chrono::seconds(2)));
  const auto statistics = pipeline.GetStatistics("line-1");
  EXPECT_EQ(statistics.mms_reports_received(), 3u);
  EXPECT_EQ(statistics.mms_events_dropped(), 1u);
  EXPECT_EQ(statistics.mms_queue_high_watermark(), 1u);
  EXPECT_EQ(statistics.data_center_batches_published(), 2u);
}

}  // namespace
