#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>

#include "CalcGroupManager.h"
#include "support/FakeDataCenter.hpp"

namespace {
using Calc::GroupManager;

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    path_ = base / ("calc_group_manager_test_tmp_" + std::to_string(ts));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path &path() const {
    return path_;
  }

private:
  std::filesystem::path path_;
};

CalcProto::UpsertGroupRequest MakeAddGroupReq(const char *groupName) {
  CalcProto::UpsertGroupRequest req;
  req.set_create_only(true);
  auto *cfg = req.mutable_config();
  cfg->set_group_name(groupName);
  auto *item = cfg->add_items();
  item->set_item_name("sum");
  item->set_operator_kind(CalcProto::OPERATOR_KIND_ADD);
  item->mutable_left_operand()->set_source_kind(CalcProto::OPERAND_SOURCE_ROUTED_INPUT);
  item->mutable_right_operand()->set_source_kind(CalcProto::OPERAND_SOURCE_ROUTED_INPUT);
  return req;
}

CalcProto::UpsertGroupRequest MakeAddConstGroupReq(const char *groupName, double constant) {
  auto req = MakeAddGroupReq(groupName);
  auto *item = req.mutable_config()->mutable_items(0);
  item->mutable_right_operand()->set_source_kind(CalcProto::OPERAND_SOURCE_CONSTANT);
  item->mutable_right_operand()->mutable_constant()->set_double_value(constant);
  return req;
}

CalcProto::UpsertGroupRequest MakeNotGroupReq(const char *groupName) {
  CalcProto::UpsertGroupRequest req;
  req.set_create_only(true);
  auto *cfg = req.mutable_config();
  cfg->set_group_name(groupName);
  auto *item = cfg->add_items();
  item->set_item_name("not1");
  item->set_operator_kind(CalcProto::OPERATOR_KIND_NOT);
  item->mutable_left_operand()->set_source_kind(CalcProto::OPERAND_SOURCE_ROUTED_INPUT);
  return req;
}

CalcProto::UpsertGroupRequest MakeDivGroupReq(const char *groupName) {
  auto req = MakeAddGroupReq(groupName);
  req.mutable_config()->mutable_items(0)->set_operator_kind(CalcProto::OPERATOR_KIND_DIV);
  return req;
}

CalcProto::UpsertGroupRequest MakeAndGroupReq(const char *groupName) {
  CalcProto::UpsertGroupRequest req;
  req.set_create_only(true);
  auto *cfg = req.mutable_config();
  cfg->set_group_name(groupName);
  auto *item = cfg->add_items();
  item->set_item_name("and1");
  item->set_operator_kind(CalcProto::OPERATOR_KIND_AND);
  item->mutable_left_operand()->set_source_kind(CalcProto::OPERAND_SOURCE_ROUTED_INPUT);
  item->mutable_right_operand()->set_source_kind(CalcProto::OPERAND_SOURCE_ROUTED_INPUT);
  return req;
}

CalcProto::UpsertGroupRequest MakeSumGroupReq(const char *groupName, int operandCount = 3) {
  CalcProto::UpsertGroupRequest req;
  req.set_create_only(true);
  auto *cfg = req.mutable_config();
  cfg->set_group_name(groupName);
  auto *item = cfg->add_items();
  item->set_item_name("aggregate");
  item->set_operator_kind(CalcProto::OPERATOR_KIND_SUM);
  for (int index = 0; index < operandCount; ++index) {
    item->add_operands()->set_source_kind(CalcProto::OPERAND_SOURCE_ROUTED_INPUT);
  }
  return req;
}

CalcProto::UpsertGroupRequest MakeAverageGroupReq(const char *groupName, uint32_t decimalPlaces) {
  auto req = MakeSumGroupReq(groupName, 3);
  auto *item = req.mutable_config()->mutable_items(0);
  item->set_operator_kind(CalcProto::OPERATOR_KIND_AVERAGE);
  item->set_decimal_places(decimalPlaces);
  return req;
}

CalcProto::UpsertGroupRequest MakeConstantAverageGroupReq(const char *groupName) {
  CalcProto::UpsertGroupRequest req;
  req.set_create_only(true);
  auto *cfg = req.mutable_config();
  cfg->set_group_name(groupName);
  auto *item = cfg->add_items();
  item->set_item_name("constant_average");
  item->set_operator_kind(CalcProto::OPERATOR_KIND_AVERAGE);
  item->set_decimal_places(2);
  for (double value : {1.0, 2.0, 2.0}) {
    auto *operand = item->add_operands();
    operand->set_source_kind(CalcProto::OPERAND_SOURCE_CONSTANT);
    operand->mutable_constant()->set_double_value(value);
  }
  return req;
}

void PublishIntPoint(FakeDataCenterState *state, uint32_t connId, const std::string &tag, int64_t value, int64_t tsMs = 0) {
  ASSERT_NE(state, nullptr);
  DataCenterProto::PublishRequest req;
  req.set_conn_id(connId);
  req.set_tag(tag);
  req.mutable_value()->set_int_value(value);
  req.set_quality(DataCenterProto::QUALITY_GOOD);
  req.set_ts_ms(tsMs);
  ASSERT_TRUE(state->Publish(req).ok());
}

bool WaitForIntLatestWithTimestamp(const FakeDataCenterState &state,
                                   uint32_t connId,
                                   const std::string &tag,
                                   int64_t expectedValue,
                                   int64_t expectedTsMs) {
  for (int i = 0; i < 50; ++i) {
    DataCenterProto::GetLatestRequest req;
    req.set_conn_id(connId);
    req.add_tags(tag);
    DataCenterProto::GetLatestResponse resp;
    if (state.GetLatest(req, &resp).ok() &&
        resp.updates_size() == 1 &&
        resp.updates(0).value().has_int_value() &&
        resp.updates(0).value().int_value() == expectedValue &&
        resp.updates(0).ts_ms() == expectedTsMs) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

void PublishDoublePoint(FakeDataCenterState *state, uint32_t connId, const std::string &tag, double value) {
  ASSERT_NE(state, nullptr);
  DataCenterProto::PublishRequest req;
  req.set_conn_id(connId);
  req.set_tag(tag);
  req.mutable_value()->set_double_value(value);
  req.set_quality(DataCenterProto::QUALITY_GOOD);
  ASSERT_TRUE(state->Publish(req).ok());
}

void PublishDecimalPoint(FakeDataCenterState *state,
                         uint32_t connId,
                         const std::string &tag,
                         const std::string &value) {
  ASSERT_NE(state, nullptr);
  DataCenterProto::PublishRequest req;
  req.set_conn_id(connId);
  req.set_tag(tag);
  req.mutable_value()->set_decimal_value(value);
  req.set_quality(DataCenterProto::QUALITY_GOOD);
  ASSERT_TRUE(state->Publish(req).ok());
}

void PublishBoolPoint(FakeDataCenterState *state, uint32_t connId, const std::string &tag, bool value) {
  ASSERT_NE(state, nullptr);
  DataCenterProto::PublishRequest req;
  req.set_conn_id(connId);
  req.set_tag(tag);
  req.mutable_value()->set_bool_value(value);
  req.set_quality(DataCenterProto::QUALITY_GOOD);
  ASSERT_TRUE(state->Publish(req).ok());
}

bool WaitForIntLatest(const FakeDataCenterState &state, uint32_t connId, const std::string &tag, int64_t expected) {
  for (int i = 0; i < 50; ++i) {
    DataCenterProto::GetLatestRequest req;
    req.set_conn_id(connId);
    req.add_tags(tag);
    DataCenterProto::GetLatestResponse resp;
    if (state.GetLatest(req, &resp).ok() &&
        resp.updates_size() == 1 &&
        resp.updates(0).value().has_int_value() &&
        resp.updates(0).value().int_value() == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForDoubleLatest(const FakeDataCenterState &state, uint32_t connId, const std::string &tag, double expected) {
  for (int i = 0; i < 50; ++i) {
    DataCenterProto::GetLatestRequest req;
    req.set_conn_id(connId);
    req.add_tags(tag);
    DataCenterProto::GetLatestResponse resp;
    if (state.GetLatest(req, &resp).ok() &&
        resp.updates_size() == 1 &&
        resp.updates(0).value().has_double_value() &&
        std::abs(resp.updates(0).value().double_value() - expected) <= 1e-9) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForDecimalLatest(const FakeDataCenterState &state,
                          uint32_t connId,
                          const std::string &tag,
                          const std::string &expected) {
  for (int i = 0; i < 50; ++i) {
    DataCenterProto::GetLatestRequest req;
    req.set_conn_id(connId);
    req.add_tags(tag);
    DataCenterProto::GetLatestResponse resp;
    if (state.GetLatest(req, &resp).ok() &&
        resp.updates_size() == 1 &&
        resp.updates(0).value().has_decimal_value() &&
        resp.updates(0).value().decimal_value() == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForBoolLatest(const FakeDataCenterState &state, uint32_t connId, const std::string &tag, bool expected) {
  for (int i = 0; i < 50; ++i) {
    DataCenterProto::GetLatestRequest req;
    req.set_conn_id(connId);
    req.add_tags(tag);
    DataCenterProto::GetLatestResponse resp;
    if (state.GetLatest(req, &resp).ok() &&
        resp.updates_size() == 1 &&
        resp.updates(0).value().has_bool_value() &&
        resp.updates(0).value().bool_value() == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

// 验证：合成结果的变化触发时标取本轮首个到达输入，而不是输入时标最大值。
TEST(CalcGroupManagerTest, ChangeTriggerUsesFirstInputTimestampForResult) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(MakeAddGroupReq("first-ts"), &info).ok());
  ASSERT_TRUE(info.state() == CalcProto::GROUP_STATE_RUNNING);
  const auto connId = info.conn_id();

  PublishIntPoint(&state, connId, "sum/left_input", 10, 100);
  PublishIntPoint(&state, connId, "sum/right_input", 20, 110);
  ASSERT_TRUE(WaitForIntLatestWithTimestamp(state, connId, "sum/result", 30, 100));
  PublishIntPoint(&state, connId, "sum/left_input", 10, 120);
  ASSERT_TRUE(WaitForIntLatestWithTimestamp(state, connId, "sum/result", 30, 120));
  EXPECT_GE(state.GetPublishCount(connId, "sum/result"), 2u);
  ASSERT_TRUE(mgr.StopGroup("first-ts").ok());
}

// 验证：三输入同一轮内重复更新只覆盖缓存，结果仍使用该轮首个输入的时标。
TEST(CalcGroupManagerTest, ChangeTriggerUsesLatestValuesAndKeepsRoundFirstTimestamp) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeSumGroupReq("first-ts-three");
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  const auto connId = info.conn_id();

  PublishIntPoint(&state, connId, "aggregate/input_1", 1, 100);
  PublishIntPoint(&state, connId, "aggregate/input_2", 2, 110);
  PublishIntPoint(&state, connId, "aggregate/input_1", 10, 120);
  EXPECT_EQ(state.GetPublishCount(connId, "aggregate/result"), 0u);
  PublishIntPoint(&state, connId, "aggregate/input_3", 3, 130);

  ASSERT_TRUE(WaitForIntLatestWithTimestamp(state, connId, "aggregate/result", 15, 100));
  ASSERT_TRUE(mgr.StopGroup("first-ts-three").ok());
}

// 验证：结果发布失败不会结束本轮，恢复后仍使用原首触发时标和最新输入值。
TEST(CalcGroupManagerTest, ChangeTriggerKeepsRoundWhenResultPublishFails) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(MakeAddGroupReq("publish-retry"), &info).ok());
  const auto connId = info.conn_id();
  state.FailPublishForTag("sum/result");
  PublishIntPoint(&state, connId, "sum/left_input", 10, 100);
  PublishIntPoint(&state, connId, "sum/right_input", 20, 110);
  for (int attempt = 0; attempt < 50 && state.GetFailedPublishCount(connId, "sum/result") == 0; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_GE(state.GetFailedPublishCount(connId, "sum/result"), 1u);
  EXPECT_EQ(state.GetPublishCount(connId, "sum/result"), 0u);

  state.AllowPublishForTag("sum/result");
  PublishIntPoint(&state, connId, "sum/left_input", 12, 120);
  ASSERT_TRUE(WaitForIntLatestWithTimestamp(state, connId, "sum/result", 32, 100));
  ASSERT_TRUE(mgr.StopGroup("publish-retry").ok());
}

// 验证：周期模式要求非零周期，非法配置不会创建计算分组。
TEST(CalcGroupManagerTest, PeriodicTriggerRejectsZeroPeriod) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAddGroupReq("period-zero");
  req.mutable_config()->set_trigger_mode(CalcProto::TRIGGER_MODE_PERIODIC);
  req.mutable_config()->set_period_ms(0);
  CalcProto::CalcGroupInfo info;
  auto status = mgr.UpsertGroup(req, &info);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：周期模式只在定时点计算，相同结果也会随新的计算时标重复发布。
TEST(CalcGroupManagerTest, PeriodicTriggerPublishesAtEachIntervalWithCalculationTimestamp) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAddGroupReq("periodic");
  req.mutable_config()->set_trigger_mode(CalcProto::TRIGGER_MODE_PERIODIC);
  req.mutable_config()->set_period_ms(60);
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  const auto connId = info.conn_id();

  PublishIntPoint(&state, connId, "sum/left_input", 10, 100);
  PublishIntPoint(&state, connId, "sum/right_input", 20, 110);

  ASSERT_TRUE(state.WaitForPublishCount(connId, "sum/result", 2, std::chrono::milliseconds(180)));
  const auto updates = state.GetPublishedUpdates(connId, "sum/result");
  ASSERT_GE(updates.size(), 2u);
  const auto &first = updates[updates.size() - 2];
  const auto &second = updates.back();
  ASSERT_TRUE(first.value().has_int_value());
  ASSERT_TRUE(second.value().has_int_value());
  EXPECT_EQ(first.value().int_value(), 30);
  EXPECT_EQ(second.value().int_value(), 30);
  EXPECT_GT(first.ts_ms(), 110);
  EXPECT_GT(second.ts_ms(), first.ts_ms());
  EXPECT_TRUE(WaitForIntLatest(state, connId, "sum/result", 30));
  ASSERT_TRUE(mgr.StopGroup("periodic").ok());
  const auto stoppedCount = state.GetPublishCount(connId, "sum/result");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(state.GetPublishCount(connId, "sum/result"), stoppedCount);
}

// 验证：分组管理器析构会等待周期线程退出，析构后不再发布结果。
TEST(CalcGroupManagerTest, DestructorStopsPeriodicThreadBeforeDependenciesAreDestroyed) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  uint32_t connId = 0;
  {
    GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
    mgr.setDataCenterStub(stub);
    auto req = MakeAddGroupReq("periodic-destructor");
    req.mutable_config()->set_trigger_mode(CalcProto::TRIGGER_MODE_PERIODIC);
    req.mutable_config()->set_period_ms(30);
    CalcProto::CalcGroupInfo info;
    ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
    connId = info.conn_id();
    PublishIntPoint(&state, connId, "sum/left_input", 10, 100);
    PublishIntPoint(&state, connId, "sum/right_input", 20, 110);
    ASSERT_TRUE(state.WaitForPublishCount(connId, "sum/result", 1, std::chrono::milliseconds(120)));
  }

  const auto destroyedCount = state.GetPublishCount(connId, "sum/result");
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  EXPECT_EQ(state.GetPublishCount(connId, "sum/result"), destroyedCount);
}

// 验证：create_only UpsertGroup 会向 DataCenter 取/建 conn_id，并在配置合法时自动启动分组功能。
TEST(CalcGroupManagerTest, UpsertGroupCreateOnlyReturnsConnIdAndAutoStartsReadyGroup) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAddConstGroupReq("calc-1", 10.0);
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  EXPECT_NE(info.conn_id(), 0u);
  EXPECT_EQ(info.state(), CalcProto::GROUP_STATE_RUNNING);
  ASSERT_EQ(info.items_size(), 1);
  EXPECT_EQ(info.items(0).left_input_tag(), "sum/left_input");
  EXPECT_EQ(info.items(0).right_input_tag(), "sum/right_input");
  EXPECT_EQ(info.items(0).result_tag(), "sum/result");
  EXPECT_TRUE(state.HasConnection("Calc", "calc-1"));
  ASSERT_TRUE(mgr.StopGroup("calc-1").ok());
}

// 验证：NOT 运算携带 right_operand 时返回 INVALID_ARGUMENT。
TEST(CalcGroupManagerTest, UpsertGroupRejectsNotWithRightOperand) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeNotGroupReq("calc-not-invalid");
  req.mutable_config()->mutable_items(0)->mutable_right_operand()->set_source_kind(CalcProto::OPERAND_SOURCE_CONSTANT);
  req.mutable_config()->mutable_items(0)->mutable_right_operand()->mutable_constant()->set_bool_value(true);

  CalcProto::CalcGroupInfo info;
  auto status = mgr.UpsertGroup(req, &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：group_name 为空、重复 item_name、二元运算缺少 right_operand 都会返回 INVALID_ARGUMENT。
TEST(CalcGroupManagerTest, UpsertGroupRejectsInvalidConfigShapes) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  CalcProto::CalcGroupInfo info;

  auto emptyNameReq = MakeAddGroupReq("");
  auto status = mgr.UpsertGroup(emptyNameReq, &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  auto duplicateReq = MakeAddGroupReq("calc-duplicate-item");
  auto *dupItem = duplicateReq.mutable_config()->add_items();
  dupItem->CopyFrom(duplicateReq.config().items(0));
  status = mgr.UpsertGroup(duplicateReq, &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  auto missingRightReq = MakeAddGroupReq("calc-missing-right");
  missingRightReq.mutable_config()->mutable_items(0)->clear_right_operand();
  status = mgr.UpsertGroup(missingRightReq, &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：SUM/AVERAGE 必须使用至少两个 operands，且不能与旧左右操作数混用。
TEST(CalcGroupManagerTest, UpsertGroupRejectsInvalidAggregateOperands) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);
  CalcProto::CalcGroupInfo info;

  auto tooFew = MakeSumGroupReq("calc-sum-too-few", 1);
  auto status = mgr.UpsertGroup(tooFew, &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  auto mixed = MakeSumGroupReq("calc-sum-mixed");
  mixed.mutable_config()->mutable_items(0)->mutable_left_operand()->set_source_kind(
      CalcProto::OPERAND_SOURCE_ROUTED_INPUT);
  status = mgr.UpsertGroup(mixed, &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：SUM 收齐三个 int64 输入后输出 int64，并返回按顺序生成的输入标签。
TEST(CalcGroupManagerTest, SumThreeIntsPublishesInt64AndInputTags) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeSumGroupReq("calc-sum-three");
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  ASSERT_EQ(info.items_size(), 1);
  ASSERT_EQ(info.items(0).input_tags_size(), 3);
  EXPECT_EQ(info.items(0).input_tags(0), "aggregate/input_1");
  EXPECT_EQ(info.items(0).input_tags(1), "aggregate/input_2");
  EXPECT_EQ(info.items(0).input_tags(2), "aggregate/input_3");

  PublishIntPoint(&state, info.conn_id(), "aggregate/input_1", 3);
  PublishIntPoint(&state, info.conn_id(), "aggregate/input_2", 4);
  PublishIntPoint(&state, info.conn_id(), "aggregate/input_3", 5);

  EXPECT_TRUE(WaitForIntLatest(state, info.conn_id(), "aggregate/result", 12));
  ASSERT_TRUE(mgr.StopGroup("calc-sum-three").ok());
}

// 验证：AVERAGE 按配置的小数位数四舍五入，并始终输出 double。
TEST(CalcGroupManagerTest, AverageUsesConfiguredDecimalPlaces) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAverageGroupReq("calc-average", 2);
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  PublishIntPoint(&state, info.conn_id(), "aggregate/input_1", 1);
  PublishIntPoint(&state, info.conn_id(), "aggregate/input_2", 2);
  PublishIntPoint(&state, info.conn_id(), "aggregate/input_3", 2);

  EXPECT_TRUE(WaitForDoubleLatest(state, info.conn_id(), "aggregate/result", 1.67));
  ASSERT_TRUE(mgr.StopGroup("calc-average").ok());
}

// 验证：精确十进制路由值 0.1 与常量 0.2 相加后，以固定 20 位小数的 decimal_value 发布。
TEST(CalcGroupManagerTest, AddDecimalOperandsPublishesFixedTwentyDecimalPlaces) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAddGroupReq("calc-add-decimal-operands");
  auto *item = req.mutable_config()->mutable_items(0);
  item->mutable_right_operand()->set_source_kind(CalcProto::OPERAND_SOURCE_CONSTANT);
  item->mutable_right_operand()->mutable_constant()->set_decimal_value("0.2");

  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  PublishDecimalPoint(&state, info.conn_id(), "sum/left_input", "0.1");
  EXPECT_TRUE(WaitForDecimalLatest(
      state, info.conn_id(), "sum/result", "0.30000000000000000000"));
  ASSERT_TRUE(mgr.StopGroup("calc-add-decimal-operands").ok());
}

// 验证：AVERAGE 按 20 位小数处理 1/3，并以固定 20 位小数的 decimal_value 发布。
TEST(CalcGroupManagerTest, AverageOneThirdPublishesTwentyDecimalPlaces) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAverageGroupReq("calc-average-one-third", 20);
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  PublishIntPoint(&state, info.conn_id(), "aggregate/input_1", 1);
  PublishIntPoint(&state, info.conn_id(), "aggregate/input_2", 0);
  PublishIntPoint(&state, info.conn_id(), "aggregate/input_3", 0);

  EXPECT_TRUE(WaitForDecimalLatest(
      state, info.conn_id(), "aggregate/result", "0.33333333333333333333"));
  ASSERT_TRUE(mgr.StopGroup("calc-average-one-third").ok());
}

// 验证：包含非法 decimal_value 的计算配置被拒绝，且不会创建计算分组。
TEST(CalcGroupManagerTest, UpsertGroupRejectsInvalidDecimalConstant) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAddGroupReq("calc-invalid-decimal");
  auto *item = req.mutable_config()->mutable_items(0);
  item->mutable_left_operand()->set_source_kind(CalcProto::OPERAND_SOURCE_CONSTANT);
  item->mutable_left_operand()->mutable_constant()->set_decimal_value("1.2.3");
  item->mutable_right_operand()->set_source_kind(CalcProto::OPERAND_SOURCE_CONSTANT);
  item->mutable_right_operand()->mutable_constant()->set_decimal_value("1");

  CalcProto::CalcGroupInfo info;
  const auto status = mgr.UpsertGroup(req, &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_THAT(status.error_message(), testing::HasSubstr("十进制"));
  EXPECT_FALSE(state.HasConnection("Calc", "calc-invalid-decimal"));
}

// 验证：缺少任一路由输入时不发布结果，并在计算项状态中指出具体等待点。
TEST(CalcGroupManagerTest, AggregateWaitsForAllInputsAndReportsMissingTag) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeSumGroupReq("calc-sum-missing");
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  PublishIntPoint(&state, info.conn_id(), "aggregate/input_1", 3);
  PublishIntPoint(&state, info.conn_id(), "aggregate/input_3", 5);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "aggregate/result"), 0u);

  CalcProto::CalcGroupInfo current;
  ASSERT_TRUE(mgr.GetGroup("calc-sum-missing", &current).ok());
  ASSERT_EQ(current.items_size(), 1);
  ASSERT_EQ(current.items(0).operand_status_size(), 3);
  EXPECT_TRUE(current.items(0).operand_status(0).ready());
  EXPECT_FALSE(current.items(0).operand_status(1).ready());
  EXPECT_EQ(current.items(0).operand_status(1).input_tag(), "aggregate/input_2");
  EXPECT_NE(current.items(0).operand_status(1).reason().find("尚未收到"), std::string::npos);
  EXPECT_NE(current.items(0).last_error().find("aggregate/input_2"), std::string::npos);

  PublishIntPoint(&state, info.conn_id(), "aggregate/input_2", 4);
  EXPECT_TRUE(WaitForIntLatest(state, info.conn_id(), "aggregate/result", 12));
  ASSERT_TRUE(mgr.StopGroup("calc-sum-missing").ok());
}

// 验证：全常量 AVERAGE 无需订阅输入，在分组运算功能启动时发布一次结果。
TEST(CalcGroupManagerTest, ConstantAveragePublishesOnStart) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeConstantAverageGroupReq("calc-constant-average");
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  EXPECT_EQ(info.state(), CalcProto::GROUP_STATE_RUNNING);
  EXPECT_EQ(state.GetSubscriptionCount(info.conn_id()), 0u);
  EXPECT_TRUE(WaitForDoubleLatest(state, info.conn_id(), "constant_average/result", 1.67));
  ASSERT_TRUE(mgr.StopGroup("calc-constant-average").ok());
}

// 验证：int + int 在未溢出时输出 int64。
TEST(CalcGroupManagerTest, AddIntAndIntPublishesInt64) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAddGroupReq("calc-add-int");
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  PublishIntPoint(&state, info.conn_id(), "sum/left_input", 3);
  PublishIntPoint(&state, info.conn_id(), "sum/right_input", 4);

  EXPECT_TRUE(WaitForIntLatest(state, info.conn_id(), "sum/result", 7));
  ASSERT_TRUE(mgr.StopGroup("calc-add-int").ok());
}

// 验证：int + double 输出 double。
TEST(CalcGroupManagerTest, AddIntAndDoublePublishesDouble) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAddGroupReq("calc-add-mixed");
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  PublishIntPoint(&state, info.conn_id(), "sum/left_input", 3);
  PublishDoublePoint(&state, info.conn_id(), "sum/right_input", 4.5);

  EXPECT_TRUE(WaitForDoubleLatest(state, info.conn_id(), "sum/result", 7.5));
  ASSERT_TRUE(mgr.StopGroup("calc-add-mixed").ok());
}

// 验证：int / int 始终输出 double。
TEST(CalcGroupManagerTest, DivIntAndIntPublishesDouble) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeDivGroupReq("calc-div");
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  PublishIntPoint(&state, info.conn_id(), "sum/left_input", 7);
  PublishIntPoint(&state, info.conn_id(), "sum/right_input", 2);

  EXPECT_TRUE(WaitForDoubleLatest(state, info.conn_id(), "sum/result", 3.5));
  ASSERT_TRUE(mgr.StopGroup("calc-div").ok());
}

// 验证：int64 溢出时自动提升为 double 后发布。
TEST(CalcGroupManagerTest, IntOverflowPromotesToDouble) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAddGroupReq("calc-overflow");
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  PublishIntPoint(&state, info.conn_id(), "sum/left_input", std::numeric_limits<int64_t>::max());
  PublishIntPoint(&state, info.conn_id(), "sum/right_input", 1);

  EXPECT_TRUE(WaitForDoubleLatest(
      state, info.conn_id(), "sum/result", static_cast<double>(std::numeric_limits<int64_t>::max()) + 1.0));
  ASSERT_TRUE(mgr.StopGroup("calc-overflow").ok());
}

// 验证：NOT 对 bool 输入可正确输出 bool 结果。
TEST(CalcGroupManagerTest, NotPublishesBoolResult) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeNotGroupReq("calc-not");
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  PublishBoolPoint(&state, info.conn_id(), "not1/left_input", true);
  EXPECT_TRUE(WaitForBoolLatest(state, info.conn_id(), "not1/result", false));
  ASSERT_TRUE(mgr.StopGroup("calc-not").ok());
}

// 验证：AND 对 bool 输入可正确输出 bool 结果。
TEST(CalcGroupManagerTest, AndPublishesBoolResult) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAndGroupReq("calc-and");
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  PublishBoolPoint(&state, info.conn_id(), "and1/left_input", true);
  PublishBoolPoint(&state, info.conn_id(), "and1/right_input", false);

  EXPECT_TRUE(WaitForBoolLatest(state, info.conn_id(), "and1/result", false));
  ASSERT_TRUE(mgr.StopGroup("calc-and").ok());
}

// 验证：逻辑运算收到非 bool 输入时不会发布结果。
TEST(CalcGroupManagerTest, LogicOperatorRejectsNumericInputAtRuntime) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeNotGroupReq("calc-not-int");
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  PublishIntPoint(&state, info.conn_id(), "not1/left_input", 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "not1/result"), 0u);
  ASSERT_TRUE(mgr.StopGroup("calc-not-int").ok());
}

// 验证：重命名分组后复用原 conn_id，并在 DataCenter 中替换连接主键。
TEST(CalcGroupManagerTest, RenameGroupReusesConnId) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAddConstGroupReq("calc-old", 2.0);
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_TRUE(mgr.StopGroup("calc-old").ok());

  CalcProto::CalcGroupInfo renamed;
  ASSERT_TRUE(mgr.RenameGroup("calc-old", "calc-new", &renamed).ok());
  EXPECT_EQ(renamed.conn_id(), info.conn_id());
  EXPECT_EQ(renamed.config().group_name(), "calc-new");
  EXPECT_FALSE(state.HasConnection("Calc", "calc-old"));
  EXPECT_TRUE(state.HasConnection("Calc", "calc-new"));
}

// 验证：运行态不允许 RenameGroup。
TEST(CalcGroupManagerTest, RenameGroupRejectsRunningGroup) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAddConstGroupReq("calc-running", 2.0);
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  CalcProto::CalcGroupInfo renamed;
  auto status = mgr.RenameGroup("calc-running", "calc-running-2", &renamed);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  ASSERT_TRUE(mgr.StopGroup("calc-running").ok());
}

// 验证：DeleteGroup 会删除内存对象并清理 DataCenter 连接。
TEST(CalcGroupManagerTest, DeleteGroupRemovesConnection) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAddConstGroupReq("calc-delete", 2.0);
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  ASSERT_TRUE(mgr.DeleteGroup("calc-delete").ok());
  EXPECT_FALSE(state.HasConnection("Calc", "calc-delete"));

  CalcProto::CalcGroupInfo deleted;
  auto status = mgr.GetGroup("calc-delete", &deleted);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：DeleteGroup 遇到 DataCenter 删除失败时会进入 PENDING_DELETE。
TEST(CalcGroupManagerTest, DeleteGroupFailureMarksPendingDelete) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
  mgr.setDataCenterStub(stub);

  auto req = MakeAddConstGroupReq("calc-delete-fail", 2.0);
  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  state.FailDeleteForConnName("calc-delete-fail");
  auto status = mgr.DeleteGroup("calc-delete-fail");
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);

  CalcProto::CalcGroupInfo current;
  ASSERT_TRUE(mgr.GetGroup("calc-delete-fail", &current).ok());
  EXPECT_EQ(current.state(), CalcProto::GROUP_STATE_PENDING_DELETE);
  EXPECT_TRUE(state.HasConnection("Calc", "calc-delete-fail"));
}

// 验证：恢复持久化配置后会复用原 conn_id，并自动恢复分组运算功能。
TEST(CalcGroupManagerTest, LoadPersistedConfigRestoresConnIdAndAutoStartsGroup) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  uint32_t savedConnId = 0;

  {
    GroupManager mgr("Calc", tempDir.path() / "conf/config.db");
    mgr.setDataCenterStub(stub);

    auto req = MakeAddConstGroupReq("calc-restore", 10.0);
    CalcProto::CalcGroupInfo info;
    ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
    savedConnId = info.conn_id();
    ASSERT_TRUE(mgr.StopGroup("calc-restore").ok());
  }

  GroupManager restored("Calc", tempDir.path() / "conf/config.db");
  restored.setDataCenterStub(stub);
  ASSERT_TRUE(restored.LoadPersistedConfig().ok());

  CalcProto::CalcGroupInfo info;
  ASSERT_TRUE(restored.GetGroup("calc-restore", &info).ok());
  EXPECT_EQ(info.conn_id(), savedConnId);
  EXPECT_EQ(info.state(), CalcProto::GROUP_STATE_RUNNING);

  PublishIntPoint(&state, info.conn_id(), "sum/left_input", 5);
  EXPECT_TRUE(WaitForDoubleLatest(state, info.conn_id(), "sum/result", 15.0));
  ASSERT_TRUE(restored.StopGroup("calc-restore").ok());
}
