#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "AVCGroupManager.h"
#include "DataCenter_mock.grpc.pb.h"
#include "support/FakeDataCenter.hpp"

namespace {
using AVC::GroupManager;

using ::testing::_;
using ::testing::Invoke;

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    path_ = base / ("avc_group_manager_test_tmp_" + std::to_string(ts));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const {
    return path_;
  }

private:
  std::filesystem::path path_;
};

AVCProto::UpsertGroupRequest MakeVoltageGroupReq(const char* groupName) {
  AVCProto::UpsertGroupRequest req;
  req.set_create_only(true);
  auto* cfg = req.mutable_config();
  cfg->set_group_name(groupName);
  cfg->mutable_voltage_meas()->set_tag("V_MEAS");
  cfg->mutable_voltage_meas()->set_unit("pu");
  cfg->mutable_voltage_cmd()->set_tag("V_CMD");
  cfg->mutable_voltage_cmd()->set_unit("pu");
  cfg->mutable_voltage_control()->set_kp(100.0);
  cfg->mutable_voltage_control()->set_deadband(0.0);

  auto* inv1 = cfg->add_members();
  inv1->set_member_name("inv-1");
  inv1->set_controllable(true);
  inv1->set_weight(1.0);
  inv1->set_q_min_kvar(0.0);
  inv1->set_q_max_kvar(100.0);
  inv1->mutable_q_meas()->set_tag("INV1_Q_MEAS");
  inv1->mutable_q_meas()->set_unit("kVar");
  inv1->mutable_q_set()->mutable_signal()->set_tag("INV1_Q_SET");
  inv1->mutable_q_set()->mutable_signal()->set_unit("kVar");
  inv1->mutable_q_set()->set_mode(AVCProto::VALUE_MODE_ABSOLUTE);

  auto* inv2 = cfg->add_members();
  inv2->set_member_name("inv-2");
  inv2->set_controllable(true);
  inv2->set_weight(1.0);
  inv2->set_q_min_kvar(0.0);
  inv2->set_q_max_kvar(100.0);
  inv2->mutable_q_meas()->set_tag("INV2_Q_MEAS");
  inv2->mutable_q_meas()->set_unit("kVar");
  inv2->mutable_q_set()->mutable_signal()->set_tag("INV2_Q_SET");
  inv2->mutable_q_set()->mutable_signal()->set_unit("kVar");
  inv2->mutable_q_set()->set_mode(AVCProto::VALUE_MODE_ABSOLUTE);

  return req;
}

AVCProto::UpsertGroupRequest MakeQTotalGroupReq(const char* groupName) {
  auto req = MakeVoltageGroupReq(groupName);
  auto* cmd = req.mutable_config()->mutable_q_total_cmd();
  cmd->mutable_signal()->set_tag("Q_CMD");
  cmd->mutable_signal()->set_unit("kVar");
  cmd->set_mode(AVCProto::VALUE_MODE_ABSOLUTE);
  return req;
}

void PublishDoublePoint(FakeDataCenterState* state, uint32_t connId, const char* tag, double value) {
  ASSERT_NE(state, nullptr);
  DataCenterProto::PublishRequest req;
  req.set_conn_id(connId);
  req.set_tag(tag);
  req.mutable_value()->set_double_value(value);
  req.set_quality(DataCenterProto::QUALITY_GOOD);
  ASSERT_TRUE(state->Publish(req).ok());
}

bool WaitForLatestDouble(const FakeDataCenterState& state, uint32_t connId, const char* tag, double expected) {
  for (int i = 0; i < 50; ++i) {
    DataCenterProto::GetLatestRequest req;
    req.set_conn_id(connId);
    req.add_tags(tag);

    DataCenterProto::GetLatestResponse resp;
    if (state.GetLatest(req, &resp).ok() && resp.updates_size() == 1 && resp.updates(0).value().has_double_value()) {
      if (std::fabs(resp.updates(0).value().double_value() - expected) <= 1e-6) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForLatestDoubleWithQuality(
    const FakeDataCenterState& state, uint32_t connId, const char* tag, double expected, DataCenterProto::Quality quality) {
  for (int i = 0; i < 50; ++i) {
    DataCenterProto::GetLatestRequest req;
    req.set_conn_id(connId);
    req.add_tags(tag);

    DataCenterProto::GetLatestResponse resp;
    if (state.GetLatest(req, &resp).ok() && resp.updates_size() == 1 && resp.updates(0).value().has_double_value()) {
      if (std::fabs(resp.updates(0).value().double_value() - expected) <= 1e-6 && resp.updates(0).quality() == quality) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForLatestIntWithQualityAndTs(
    const FakeDataCenterState& state, uint32_t connId, const char* tag, int64_t expected, DataCenterProto::Quality quality, int64_t tsMs) {
  for (int i = 0; i < 50; ++i) {
    DataCenterProto::GetLatestRequest req;
    req.set_conn_id(connId);
    req.add_tags(tag);

    DataCenterProto::GetLatestResponse resp;
    if (state.GetLatest(req, &resp).ok() && resp.updates_size() == 1 && resp.updates(0).value().has_int_value()) {
      if (resp.updates(0).value().int_value() == expected && resp.updates(0).quality() == quality && resp.updates(0).ts_ms() == tsMs) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForPublishCount(const FakeDataCenterState& state, uint32_t connId, const char* tag, size_t expected) {
  for (int i = 0; i < 50; ++i) {
    if (state.GetPublishCount(connId, tag) >= expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForSubscriptionCount(const FakeDataCenterState& state, uint32_t connId, size_t expected) {
  for (int i = 0; i < 50; ++i) {
    if (state.GetSubscriptionCount(connId) >= expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

// 验证：create_only UpsertGroup 会向 DataCenter 取/建 conn_id，并在配置合法时自动启动控制组内功能。
TEST(AvcGroupManagerTest, UpsertGroupCreateOnlyReturnsConnIdAndAutoStartsReadyGroup) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);
  auto req = MakeVoltageGroupReq("g-1");

  AVCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  EXPECT_NE(info.conn_id(), 0u);
  EXPECT_EQ(info.state(), AVCProto::GROUP_STATE_RUNNING);
  EXPECT_TRUE(state.HasConnection("AVC", "g-1"));
  ASSERT_TRUE(mgr.StopGroup("g-1").ok());
}

// 验证：UpsertGroup 返回 AVC 自动生成的默认点列表，供上位机直接发现并配置路由。
TEST(AvcGroupManagerTest, UpsertGroupReturnsDefaultPointInfos) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);

  AVCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(MakeVoltageGroupReq("g-default-points"), &info).ok());
  ASSERT_EQ(info.default_points_size(), 10);

  std::unordered_map<std::string, AVCProto::DefaultPointKind> defaultPoints;
  for (const auto& point : info.default_points()) {
    defaultPoints.emplace(point.tag(), point.kind());
  }
  EXPECT_EQ(defaultPoints["理论可调无功下限"], AVCProto::DEFAULT_POINT_KIND_THEORETICAL_LOWER);
  EXPECT_EQ(defaultPoints["理论可调无功上限"], AVCProto::DEFAULT_POINT_KIND_THEORETICAL_UPPER);
  EXPECT_EQ(defaultPoints["当前可调无功下限"], AVCProto::DEFAULT_POINT_KIND_DYNAMIC_LOWER);
  EXPECT_EQ(defaultPoints["当前可调无功上限"], AVCProto::DEFAULT_POINT_KIND_DYNAMIC_UPPER);
  EXPECT_EQ(defaultPoints["调节返回值"], AVCProto::DEFAULT_POINT_KIND_COMMAND_ECHO);
  EXPECT_EQ(defaultPoints["当前电压"], AVCProto::DEFAULT_POINT_KIND_CURRENT_VOLTAGE);
  EXPECT_EQ(defaultPoints["总无功目标"], AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_TARGET);
  EXPECT_EQ(defaultPoints["总无功实测"], AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_MEAS);
  EXPECT_EQ(defaultPoints["总无功偏差"], AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_ERROR);
  EXPECT_EQ(defaultPoints["电压偏差"], AVCProto::DEFAULT_POINT_KIND_VOLTAGE_ERROR);

  ASSERT_TRUE(mgr.StopGroup("g-default-points").ok());
}

// 验证：默认限值点会注册到 DataCenter 标签注册表并在不可控缺测时先发布 BAD 质量，量测到来后转为 GOOD。
TEST(AvcGroupManagerTest, UpsertGroupRegistersAndPublishesDefaultLimitPoints) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);
  auto req = MakeVoltageGroupReq("g-default-limit");
  req.mutable_config()->mutable_members(0)->set_controllable(false);
  req.mutable_config()->mutable_members(1)->set_q_min_kvar(10.0);
  req.mutable_config()->mutable_members(1)->set_q_max_kvar(80.0);

  AVCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_NE(info.conn_id(), 0u);

  EXPECT_TRUE(WaitForLatestDoubleWithQuality(
      state, info.conn_id(), "理论可调无功下限", 10.0, DataCenterProto::QUALITY_GOOD));
  EXPECT_TRUE(WaitForLatestDoubleWithQuality(
      state, info.conn_id(), "理论可调无功上限", 80.0, DataCenterProto::QUALITY_GOOD));
  EXPECT_TRUE(WaitForLatestDoubleWithQuality(
      state, info.conn_id(), "当前可调无功下限", 10.0, DataCenterProto::QUALITY_BAD));
  EXPECT_TRUE(WaitForLatestDoubleWithQuality(
      state, info.conn_id(), "当前可调无功上限", 80.0, DataCenterProto::QUALITY_BAD));

  PublishDoublePoint(&state, info.conn_id(), "INV1_Q_MEAS", 20.0);
  EXPECT_TRUE(WaitForLatestDoubleWithQuality(
      state, info.conn_id(), "当前可调无功下限", 30.0, DataCenterProto::QUALITY_GOOD));
  EXPECT_TRUE(WaitForLatestDoubleWithQuality(
      state, info.conn_id(), "当前可调无功上限", 100.0, DataCenterProto::QUALITY_GOOD));

  ASSERT_TRUE(mgr.StopGroup("g-default-limit").ok());
}

// 验证：目标电压模式在输入点更新后会发布当前电压、总无功结果点和成员无功设定。
TEST(AvcGroupManagerTest, VoltageModePublishesTelemetryAndMemberSetpoints) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);

  AVCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(MakeVoltageGroupReq("g-voltage"), &info).ok());
  ASSERT_TRUE(WaitForSubscriptionCount(state, info.conn_id(), 1));

  PublishDoublePoint(&state, info.conn_id(), "V_CMD", 1.0);
  PublishDoublePoint(&state, info.conn_id(), "V_MEAS", 0.9);
  PublishDoublePoint(&state, info.conn_id(), "INV1_Q_MEAS", 0.0);
  PublishDoublePoint(&state, info.conn_id(), "INV2_Q_MEAS", 0.0);

  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "当前电压", 0.9));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "总无功实测", 0.0));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "总无功目标", 10.0));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "总无功偏差", 10.0));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "电压偏差", 0.1));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "INV1_Q_SET", 5.0));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "INV2_Q_SET", 5.0));

  ASSERT_TRUE(mgr.StopGroup("g-voltage").ok());
}

// 验证：总无功模式在命令更新后会发布总无功结果点，但不会发布电压偏差点。
TEST(AvcGroupManagerTest, QTotalModePublishesReactiveTelemetryWithoutVoltageError) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);

  AVCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(MakeQTotalGroupReq("g-q"), &info).ok());
  ASSERT_TRUE(WaitForSubscriptionCount(state, info.conn_id(), 1));

  PublishDoublePoint(&state, info.conn_id(), "Q_CMD", 20.0);
  PublishDoublePoint(&state, info.conn_id(), "V_MEAS", 0.98);
  PublishDoublePoint(&state, info.conn_id(), "INV1_Q_MEAS", 2.0);
  PublishDoublePoint(&state, info.conn_id(), "INV2_Q_MEAS", 3.0);

  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "当前电压", 0.98));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "总无功实测", 5.0));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "总无功目标", 20.0));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "总无功偏差", 15.0));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "电压偏差"), 0u);

  ASSERT_TRUE(mgr.StopGroup("g-q").ok());
}

// 验证：命令点更新会回显到“调节返回值”默认点，并保留质量与时间戳。
TEST(AvcGroupManagerTest, CommandEchoPublishesRawValueQualityAndTimestamp) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);

  AVCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(MakeQTotalGroupReq("g-echo"), &info).ok());

  DataCenterProto::PublishRequest req;
  req.set_conn_id(info.conn_id());
  req.set_tag("Q_CMD");
  req.mutable_value()->set_int_value(12);
  req.set_quality(DataCenterProto::QUALITY_BAD);
  req.set_ts_ms(123456);
  ASSERT_TRUE(state.Publish(req).ok());

  EXPECT_TRUE(WaitForLatestIntWithQualityAndTs(
      state, info.conn_id(), "调节返回值", 12, DataCenterProto::QUALITY_BAD, 123456));

  ASSERT_TRUE(mgr.StopGroup("g-echo").ok());
}

// 验证：RenameGroup 成功后保留 conn_id，旧名字失效，新名字可继续查询。
TEST(AvcGroupManagerTest, RenameGroupKeepsConnId) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);
  auto req = MakeVoltageGroupReq("g-old");

  AVCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_TRUE(mgr.StopGroup("g-old").ok());

  AVCProto::GroupInfo renamed;
  ASSERT_TRUE(mgr.RenameGroup("g-old", "g-new", &renamed).ok());
  EXPECT_EQ(renamed.conn_id(), info.conn_id());
  EXPECT_FALSE(state.HasConnection("AVC", "g-old"));
  EXPECT_TRUE(state.HasConnection("AVC", "g-new"));
}

// 验证：RenameGroup 在 old_group_name 与 new_group_name 相同时按幂等返回当前 GroupInfo。
TEST(AvcGroupManagerTest, RenameGroupSameNameIsIdempotent) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);

  AVCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(MakeVoltageGroupReq("g-idempotent"), &info).ok());
  ASSERT_TRUE(mgr.StopGroup("g-idempotent").ok());

  AVCProto::GroupInfo renamed;
  ASSERT_TRUE(mgr.RenameGroup("g-idempotent", "g-idempotent", &renamed).ok());
  EXPECT_EQ(renamed.conn_id(), info.conn_id());
  EXPECT_EQ(renamed.config().group_name(), "g-idempotent");
  EXPECT_EQ(renamed.state(), AVCProto::GROUP_STATE_STOPPED);
  EXPECT_TRUE(state.HasConnection("AVC", "g-idempotent"));
}

// 验证：运行态不允许 RenameGroup。
TEST(AvcGroupManagerTest, RenameGroupRejectsRunningGroup) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);

  AVCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(MakeVoltageGroupReq("g-running"), &info).ok());

  AVCProto::GroupInfo renamed;
  auto status = mgr.RenameGroup("g-running", "g-running-2", &renamed);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  ASSERT_TRUE(mgr.StopGroup("g-running").ok());
}

// 验证：待删除态不允许 RenameGroup。
TEST(AvcGroupManagerTest, RenameGroupRejectsPendingDeleteGroup) {
  FakeDataCenterState state;
  state.FailDeleteForConnName("g-pending");
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);

  AVCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(MakeVoltageGroupReq("g-pending"), &info).ok());
  auto deleteStatus = mgr.DeleteGroup("g-pending");
  ASSERT_EQ(deleteStatus.error_code(), grpc::StatusCode::INTERNAL);

  AVCProto::GroupInfo renamed;
  auto status = mgr.RenameGroup("g-pending", "g-pending-2", &renamed);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：RenameGroup 的目标组名已存在时返回 ALREADY_EXISTS。
TEST(AvcGroupManagerTest, RenameGroupRejectsExistingTargetName) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);

  AVCProto::GroupInfo infoA;
  ASSERT_TRUE(mgr.UpsertGroup(MakeVoltageGroupReq("g-a"), &infoA).ok());
  ASSERT_TRUE(mgr.StopGroup("g-a").ok());

  AVCProto::GroupInfo infoB;
  ASSERT_TRUE(mgr.UpsertGroup(MakeVoltageGroupReq("g-b"), &infoB).ok());
  ASSERT_TRUE(mgr.StopGroup("g-b").ok());

  AVCProto::GroupInfo renamed;
  auto status = mgr.RenameGroup("g-a", "g-b", &renamed);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：RenameGroup 在旧控制组不存在时返回 NOT_FOUND。
TEST(AvcGroupManagerTest, RenameGroupReturnsNotFoundWhenOldGroupMissing) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);

  AVCProto::GroupInfo renamed;
  auto status = mgr.RenameGroup("missing-group", "missing-group-2", &renamed);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：DeleteGroup 会调用 DataCenter.DeleteConnection，并移除本地 group 配置。
TEST(AvcGroupManagerTest, DeleteGroupCallsDataCenterDeleteAndRemovesLocal) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);

  AVCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(MakeVoltageGroupReq("g-del"), &info).ok());
  ASSERT_TRUE(state.HasConnection("AVC", "g-del"));

  ASSERT_TRUE(mgr.DeleteGroup("g-del").ok());
  EXPECT_FALSE(state.HasConnection("AVC", "g-del"));

  AVCProto::GroupInfo got;
  auto status = mgr.GetGroup("g-del", &got);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：当 DataCenter 删除失败时，DeleteGroup 标记 PENDING_DELETE 且保留本地配置以便重试。
TEST(AvcGroupManagerTest, DeleteGroupFailureMarksPendingDeleteAndKeepsLocal) {
  FakeDataCenterState state;
  state.FailDeleteForConnName("g-fail");
  auto stub = MakeStub(&state);

  GroupManager mgr("AVC");
  mgr.setDataCenterStub(stub);

  AVCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(MakeVoltageGroupReq("g-fail"), &info).ok());

  auto status = mgr.DeleteGroup("g-fail");
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);

  AVCProto::GroupInfo got;
  ASSERT_TRUE(mgr.GetGroup("g-fail", &got).ok());
  EXPECT_EQ(got.state(), AVCProto::GROUP_STATE_PENDING_DELETE);
  EXPECT_FALSE(got.last_error().empty());
}

// 验证：持久化恢复后会重新取回 conn_id，并自动启动满足条件的控制组。
TEST(AvcGroupManagerTest, LoadPersistedConfigRestoresConnIdAndAutoStartsGroup) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  const auto groupsPath = tempDir.path() / "conf/AVC/groups.pb";
  std::filesystem::create_directories(groupsPath.parent_path());

  {
    GroupManager writer("AVC", groupsPath);
    writer.setDataCenterStub(stub);

    AVCProto::GroupInfo info;
    ASSERT_TRUE(writer.UpsertGroup(MakeVoltageGroupReq("g-restore"), &info).ok());
    ASSERT_TRUE(writer.StopGroup("g-restore").ok());
  }

  GroupManager reader("AVC", groupsPath);
  reader.setDataCenterStub(stub);
  ASSERT_TRUE(reader.LoadPersistedConfig().ok());

  AVCProto::GroupInfo info;
  ASSERT_TRUE(reader.GetGroup("g-restore", &info).ok());
  EXPECT_EQ(info.state(), AVCProto::GROUP_STATE_RUNNING);
  ASSERT_TRUE(reader.StopGroup("g-restore").ok());
}
