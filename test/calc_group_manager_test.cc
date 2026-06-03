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

void PublishIntPoint(FakeDataCenterState *state, uint32_t connId, const std::string &tag, int64_t value) {
  ASSERT_NE(state, nullptr);
  DataCenterProto::PublishRequest req;
  req.set_conn_id(connId);
  req.set_tag(tag);
  req.mutable_value()->set_int_value(value);
  req.set_quality(DataCenterProto::QUALITY_GOOD);
  ASSERT_TRUE(state->Publish(req).ok());
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
