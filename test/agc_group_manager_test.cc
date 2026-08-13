#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "AGCGroupManager.h"
#include "DataCenter_mock.grpc.pb.h"
#include "support/FakeDataCenter.hpp"

namespace {
using AGC::GroupManager;

using ::testing::_;
using ::testing::Invoke;

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    path_ = base / ("agc_group_manager_test_tmp_" + std::to_string(ts));
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

AGCProto::UpsertGroupRequest MakeGroupReq(const char *groupName) {
  AGCProto::UpsertGroupRequest req;
  req.set_create_only(true);
  auto *cfg = req.mutable_config();
  cfg->set_group_name(groupName);
  cfg->mutable_p_cmd()->mutable_signal()->set_tag("P_CMD");
  cfg->mutable_p_cmd()->mutable_signal()->set_unit("kW");
  cfg->mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  cfg->mutable_outputs()->mutable_p_total_meas()->set_tag("P_TOTAL");
  cfg->mutable_outputs()->mutable_p_total_meas()->set_unit("kW");

  auto *inv1 = cfg->add_members();
  inv1->set_member_name("inv-1");
  inv1->set_controllable(true);
  inv1->set_capacity_kw(50);
  inv1->set_weight(50);
  inv1->mutable_p_meas()->set_tag("INV1_P_MEAS");
  inv1->mutable_p_meas()->set_unit("kW");
  inv1->mutable_p_set()->mutable_signal()->set_tag("INV1_P_SET");
  inv1->mutable_p_set()->mutable_signal()->set_unit("kW");
  inv1->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  auto *inv2 = cfg->add_members();
  inv2->set_member_name("inv-2");
  inv2->set_controllable(true);
  inv2->set_capacity_kw(100);
  inv2->set_weight(100);
  inv2->mutable_p_meas()->set_tag("INV2_P_MEAS");
  inv2->mutable_p_meas()->set_unit("kW");
  inv2->mutable_p_set()->mutable_signal()->set_tag("INV2_P_SET");
  inv2->mutable_p_set()->mutable_signal()->set_unit("kW");
  inv2->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  return req;
}

void PublishDoublePoint(FakeDataCenterState *state, uint32_t connId, const char *tag, double value) {
  ASSERT_NE(state, nullptr);
  DataCenterProto::PublishRequest req;
  req.set_conn_id(connId);
  req.set_tag(tag);
  req.mutable_value()->set_double_value(value);
  req.set_quality(DataCenterProto::QUALITY_GOOD);
  ASSERT_TRUE(state->Publish(req).ok());
}

bool WaitForLatestDouble(const FakeDataCenterState &state, uint32_t connId, const char *tag, double expected) {
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
    const FakeDataCenterState &state, uint32_t connId, const char *tag, double expected, DataCenterProto::Quality quality) {
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

bool WaitForLatestDoubleWithQualityAndTs(
    const FakeDataCenterState &state, uint32_t connId, const char *tag, double expected, DataCenterProto::Quality quality, int64_t tsMs) {
  for (int i = 0; i < 50; ++i) {
    DataCenterProto::GetLatestRequest req;
    req.set_conn_id(connId);
    req.add_tags(tag);

    DataCenterProto::GetLatestResponse resp;
    if (state.GetLatest(req, &resp).ok() && resp.updates_size() == 1 && resp.updates(0).value().has_double_value()) {
      if (std::fabs(resp.updates(0).value().double_value() - expected) <= 1e-6 && resp.updates(0).quality() == quality &&
          resp.updates(0).ts_ms() == tsMs) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForPublishCount(const FakeDataCenterState &state, uint32_t connId, const char *tag, size_t expected) {
  for (int i = 0; i < 50; ++i) {
    if (state.GetPublishCount(connId, tag) >= expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForSubscriptionCount(const FakeDataCenterState &state, uint32_t connId, size_t expected) {
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
TEST(AgcGroupManagerTest, UpsertGroupCreateOnlyReturnsConnIdAndAutoStartsReadyGroup) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-1");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  EXPECT_NE(info.conn_id(), 0u);
  EXPECT_EQ(info.state(), AGCProto::GROUP_STATE_RUNNING);
  EXPECT_EQ(info.config().group_name(), "g-1");
  EXPECT_TRUE(state.HasConnection("AGC", "g-1"));
  ASSERT_TRUE(mgr.StopGroup("g-1").ok());
}

// 验证：UpsertGroup 返回 AGC 自动生成的默认点列表，供上位机直接发现并配置路由。
TEST(AgcGroupManagerTest, UpsertGroupReturnsDefaultPointInfos) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(MakeGroupReq("g-default-points"), &info).ok());
  ASSERT_EQ(info.default_points_size(), 6);

  std::unordered_map<std::string, AGCProto::DefaultPointKind> defaultPoints;
  for (const auto &point : info.default_points()) {
    defaultPoints.emplace(point.tag(), point.kind());
  }
  EXPECT_EQ(defaultPoints["理论可调有功下限"], AGCProto::DEFAULT_POINT_KIND_THEORETICAL_LOWER);
  EXPECT_EQ(defaultPoints["理论可调有功上限"], AGCProto::DEFAULT_POINT_KIND_THEORETICAL_UPPER);
  EXPECT_EQ(defaultPoints["当前可调有功下限"], AGCProto::DEFAULT_POINT_KIND_DYNAMIC_LOWER);
  EXPECT_EQ(defaultPoints["当前可调有功上限"], AGCProto::DEFAULT_POINT_KIND_DYNAMIC_UPPER);
  EXPECT_EQ(defaultPoints["调节返回值"], AGCProto::DEFAULT_POINT_KIND_COMMAND_ECHO);
  EXPECT_EQ(defaultPoints["AGC装机容量"], AGCProto::DEFAULT_POINT_KIND_INSTALLED_CAPACITY);

  ASSERT_TRUE(mgr.StopGroup("g-default-points").ok());
}

// 验证：任一成员缺少正的额定容量时，AGC 拒绝创建控制组。
TEST(AgcGroupManagerTest, UpsertGroupRejectsMissingMemberCapacity) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-missing-capacity");
  req.mutable_config()->mutable_members(1)->set_capacity_kw(0.0);

  AGCProto::GroupInfo info;
  const auto status = mgr.UpsertGroup(req, &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_THAT(status.error_message(), ::testing::HasSubstr("inv-2"));
}

// 验证：成员额定容量必须是有限数值，NaN/Inf 不能绕过配置校验。
TEST(AgcGroupManagerTest, UpsertGroupRejectsNonFiniteMemberCapacity) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-non-finite-capacity");
  req.mutable_config()->mutable_members(0)->set_capacity_kw(std::numeric_limits<double>::infinity());

  AGCProto::GroupInfo info;
  const auto status = mgr.UpsertGroup(req, &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_THAT(status.error_message(), ::testing::HasSubstr("inv-1"));
}

// 验证：AGC装机容量默认点发布所有成员额定容量之和。
TEST(AgcGroupManagerTest, PublishesInstalledCapacityDefaultPoint) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  AGCProto::GroupInfo info;
  auto req = MakeGroupReq("g-installed-capacity");
  req.mutable_config()->mutable_members(1)->set_controllable(false);
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "AGC装机容量", 150.0));
  ASSERT_TRUE(mgr.StopGroup("g-installed-capacity").ok());
}

// 验证：当 DataCenter 已存在相同 (module_name, conn_name) 时，create_only UpsertGroup 返回 ALREADY_EXISTS。
TEST(AgcGroupManagerTest, UpsertGroupCreateOnlyRejectsWhenDataCenterAlreadyHasKey) {
  FakeDataCenterState state;
  state.AddConnection(42, "AGC", "dup");
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("dup");

  AGCProto::GroupInfo info;
  auto st = mgr.UpsertGroup(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：DeleteGroup 会调用 DataCenter.DeleteConnection，并移除本地 group 配置。
TEST(AgcGroupManagerTest, DeleteGroupCallsDataCenterDeleteAndRemovesLocal) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-del");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_TRUE(state.HasConnection("AGC", "g-del"));

  ASSERT_TRUE(mgr.DeleteGroup("g-del").ok());
  EXPECT_FALSE(state.HasConnection("AGC", "g-del"));

  AGCProto::GroupInfo got;
  auto st = mgr.GetGroup("g-del", &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：默认限值点会注册到 DataCenter 标签注册表并在不可控缺测时先发布 BAD 质量，量测到来后转为 GOOD。
TEST(AgcGroupManagerTest, UpsertGroupRegistersAndPublishesDefaultLimitPoints) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-default-limit");
  req.mutable_config()->mutable_members(0)->set_controllable(false);
  req.mutable_config()->mutable_members(1)->set_min_kw(10.0);
  req.mutable_config()->mutable_members(1)->set_max_kw(80.0);

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_NE(info.conn_id(), 0u);

  EXPECT_TRUE(WaitForLatestDoubleWithQuality(
      state, info.conn_id(), "理论可调有功下限", 10.0, DataCenterProto::QUALITY_GOOD));
  EXPECT_TRUE(WaitForLatestDoubleWithQuality(
      state, info.conn_id(), "理论可调有功上限", 80.0, DataCenterProto::QUALITY_GOOD));
  EXPECT_TRUE(WaitForLatestDoubleWithQuality(
      state, info.conn_id(), "当前可调有功下限", 10.0, DataCenterProto::QUALITY_BAD));
  EXPECT_TRUE(WaitForLatestDoubleWithQuality(
      state, info.conn_id(), "当前可调有功上限", 80.0, DataCenterProto::QUALITY_BAD));

  PublishDoublePoint(&state, info.conn_id(), "INV1_P_MEAS", 20.0);
  EXPECT_TRUE(WaitForLatestDoubleWithQuality(
      state, info.conn_id(), "当前可调有功下限", 30.0, DataCenterProto::QUALITY_GOOD));
  EXPECT_TRUE(WaitForLatestDoubleWithQuality(
      state, info.conn_id(), "当前可调有功上限", 100.0, DataCenterProto::QUALITY_GOOD));

  ASSERT_TRUE(mgr.StopGroup("g-default-limit").ok());
}

// 验证：当 DataCenter 删除失败时，DeleteGroup 标记 PENDING_DELETE 且保留本地配置以便重试。
TEST(AgcGroupManagerTest, DeleteGroupFailureMarksPendingDeleteAndKeepsLocal) {
  FakeDataCenterState state;
  state.FailDeleteForConnName("g-fail");
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-fail");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());

  auto st = mgr.DeleteGroup("g-fail");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);

  AGCProto::GroupInfo got;
  ASSERT_TRUE(mgr.GetGroup("g-fail", &got).ok());
  EXPECT_EQ(got.state(), AGCProto::GROUP_STATE_PENDING_DELETE);
  EXPECT_FALSE(got.last_error().empty());
}

// 验证：StopGroup 不会把 PENDING_DELETE 状态清回 STOPPED。
TEST(AgcGroupManagerTest, StopGroupKeepsPendingDeleteState) {
  FakeDataCenterState state;
  state.FailDeleteForConnName("g-pending-stop");
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-pending-stop");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_EQ(mgr.DeleteGroup("g-pending-stop").error_code(), grpc::StatusCode::INTERNAL);
  ASSERT_TRUE(mgr.StopGroup("g-pending-stop").ok());

  AGCProto::GroupInfo got;
  ASSERT_TRUE(mgr.GetGroup("g-pending-stop", &got).ok());
  EXPECT_EQ(got.state(), AGCProto::GROUP_STATE_PENDING_DELETE);
}

// 验证：当 ValueSpec 使用 BASE_TAG 时，UpsertGroup 会把 base_tag 一并注册到 DataCenter 连接标签注册表。
TEST(AgcGroupManagerTest, UpsertGroupRegistersBaseTagToDataCenterConnTags) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  EXPECT_CALL(*stub, UpsertConnTags(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *, const DataCenterProto::UpsertConnTagsRequest &req, DataCenterProto::Empty *) {
        EXPECT_NE(req.conn_id(), 0u);
        EXPECT_TRUE(req.replace());

        std::unordered_set<std::string> tags;
        for (const auto &t : req.tags()) {
          tags.emplace(t);
        }
        EXPECT_TRUE(tags.contains("P_CMD"));
        EXPECT_TRUE(tags.contains("P_BASE"));
        EXPECT_TRUE(tags.contains("INV1_P_MEAS"));
        EXPECT_TRUE(tags.contains("INV1_P_SET"));
        EXPECT_TRUE(tags.contains("INV2_P_MEAS"));
        EXPECT_TRUE(tags.contains("INV2_P_SET"));
        EXPECT_TRUE(tags.contains("P_TOTAL"));
        EXPECT_TRUE(tags.contains("理论可调有功下限"));
        EXPECT_TRUE(tags.contains("理论可调有功上限"));
        EXPECT_TRUE(tags.contains("当前可调有功下限"));
        EXPECT_TRUE(tags.contains("当前可调有功上限"));
        EXPECT_TRUE(tags.contains("调节返回值"));
        EXPECT_TRUE(tags.contains("AGC装机容量"));
        return grpc::Status::OK;
      }));

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-base");
  req.mutable_config()->mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_DELTA);
  req.mutable_config()->mutable_p_cmd()->set_delta_base(AGCProto::DELTA_BASE_BASE_TAG);
  req.mutable_config()->mutable_p_cmd()->set_base_tag("P_BASE");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
}

// 验证：保留给 AGC 默认点的 tag 不能被用户配置点复用。
TEST(AgcGroupManagerTest, UpsertGroupRejectsReservedDefaultPointTag) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-reserved-tag");
  req.mutable_config()->mutable_p_cmd()->mutable_signal()->set_tag("理论可调有功上限");

  AGCProto::GroupInfo info;
  auto st = mgr.UpsertGroup(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：实时命令输入会将工程量、质量与时间戳回显到调节返回值默认点。
TEST(AgcGroupManagerTest, RealtimeCommandPublishesEngineeringCommandEcho) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-cmd-echo");
  req.mutable_config()->mutable_p_cmd()->mutable_signal()->set_scale(2.0);
  req.mutable_config()->mutable_p_cmd()->mutable_signal()->set_offset(5.0);

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_TRUE(WaitForSubscriptionCount(state, info.conn_id(), 1));

  DataCenterProto::PublishRequest publishReq;
  publishReq.set_conn_id(info.conn_id());
  publishReq.set_tag("P_CMD");
  publishReq.mutable_value()->set_int_value(500);
  publishReq.set_ts_ms(123456);
  publishReq.set_quality(DataCenterProto::QUALITY_BAD);
  ASSERT_TRUE(state.Publish(publishReq).ok());

  EXPECT_TRUE(WaitForLatestDoubleWithQualityAndTs(
      state, info.conn_id(), "调节返回值", 1005.0, DataCenterProto::QUALITY_BAD, 123456));

  ASSERT_TRUE(mgr.StopGroup("g-cmd-echo").ok());
}

// 验证：缺少命令 tag 会返回 INVALID_ARGUMENT。
TEST(AgcGroupManagerTest, UpsertGroupRejectsMissingCommandTag) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-missing-cmd");
  req.mutable_config()->mutable_p_cmd()->mutable_signal()->set_tag("");

  AGCProto::GroupInfo info;
  auto st = mgr.UpsertGroup(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：重复的 member_name 会返回 INVALID_ARGUMENT。
TEST(AgcGroupManagerTest, UpsertGroupRejectsDuplicateMemberName) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-dup-member");
  req.mutable_config()->mutable_members(1)->set_member_name("inv-1");

  AGCProto::GroupInfo info;
  auto st = mgr.UpsertGroup(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：缺少成员量测点 tag 会返回 INVALID_ARGUMENT。
TEST(AgcGroupManagerTest, UpsertGroupRejectsMissingMemberMeasTag) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-missing-meas");
  req.mutable_config()->mutable_members(0)->mutable_p_meas()->set_tag("");

  AGCProto::GroupInfo info;
  auto st = mgr.UpsertGroup(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：可控成员缺少设定点时返回 INVALID_ARGUMENT。
TEST(AgcGroupManagerTest, UpsertGroupRejectsMissingMemberSetpointForControllable) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-missing-set");
  req.mutable_config()->mutable_members(0)->clear_p_set();

  AGCProto::GroupInfo info;
  auto st = mgr.UpsertGroup(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：StopGroup 在 STOPPED 状态下幂等返回 OK。
TEST(AgcGroupManagerTest, StopGroupIsIdempotentWhenStopped) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-stop-idem");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_TRUE(mgr.StopGroup("g-stop-idem").ok());
  ASSERT_TRUE(mgr.StopGroup("g-stop-idem").ok());

  AGCProto::GroupInfo got;
  ASSERT_TRUE(mgr.GetGroup("g-stop-idem", &got).ok());
  EXPECT_EQ(got.state(), AGCProto::GROUP_STATE_STOPPED);
}

// 验证：控制组运行中不允许更新配置。
TEST(AgcGroupManagerTest, UpsertGroupRejectsWhenRunning) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-running");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_TRUE(mgr.StartGroup("g-running").ok());

  auto updateReq = MakeGroupReq("g-running");
  updateReq.set_create_only(false);
  updateReq.mutable_config()->mutable_members(0)->set_weight(10);

  AGCProto::GroupInfo updated;
  auto st = mgr.UpsertGroup(updateReq, &updated);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  ASSERT_TRUE(mgr.StopGroup("g-running").ok());
}

// 验证：PENDING_DELETE 状态下不允许启动控制组。
TEST(AgcGroupManagerTest, StartGroupRejectsWhenPendingDelete) {
  FakeDataCenterState state;
  state.FailDeleteForConnName("g-pending");
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-pending");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  auto del = mgr.DeleteGroup("g-pending");
  EXPECT_EQ(del.error_code(), grpc::StatusCode::INTERNAL);

  auto st = mgr.StartGroup("g-pending");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：显式 StartGroup 在预置初始输入快照后会触发一次控制，并且不会按固定周期重复下发。
TEST(AgcGroupManagerTest, ExplicitStartUsesInitialSnapshotWithoutPeriodicRepublish) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-initial-snapshot");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_TRUE(mgr.StopGroup("g-initial-snapshot").ok());
  PublishDoublePoint(&state, info.conn_id(), "P_CMD", 60.0);
  PublishDoublePoint(&state, info.conn_id(), "INV1_P_MEAS", 10.0);
  PublishDoublePoint(&state, info.conn_id(), "INV2_P_MEAS", 20.0);

  ASSERT_TRUE(mgr.StartGroup("g-initial-snapshot").ok());
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "P_TOTAL", 30.0));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "INV1_P_SET", 20.0));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "INV2_P_SET", 40.0));

  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "P_TOTAL"), 1u);
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "INV1_P_SET"), 1u);
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "INV2_P_SET"), 1u);

  ASSERT_TRUE(mgr.StopGroup("g-initial-snapshot").ok());
}

// 验证：控制组重新启动时，即使初始快照值未变化，也会重新触发一次事件控制。
TEST(AgcGroupManagerTest, RestartGroupReusesInitialSnapshotWithoutWaitingForNewInput) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-restart-snapshot");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_TRUE(mgr.StopGroup("g-restart-snapshot").ok());
  PublishDoublePoint(&state, info.conn_id(), "P_CMD", 60.0);
  PublishDoublePoint(&state, info.conn_id(), "INV1_P_MEAS", 10.0);
  PublishDoublePoint(&state, info.conn_id(), "INV2_P_MEAS", 20.0);

  ASSERT_TRUE(mgr.StartGroup("g-restart-snapshot").ok());
  ASSERT_TRUE(WaitForPublishCount(state, info.conn_id(), "INV1_P_SET", 1u));
  ASSERT_TRUE(mgr.StopGroup("g-restart-snapshot").ok());

  ASSERT_TRUE(mgr.StartGroup("g-restart-snapshot").ok());
  EXPECT_TRUE(WaitForPublishCount(state, info.conn_id(), "INV1_P_SET", 2u));
  EXPECT_TRUE(WaitForPublishCount(state, info.conn_id(), "INV2_P_SET", 2u));

  ASSERT_TRUE(mgr.StopGroup("g-restart-snapshot").ok());
}

// 验证：控制组启动后，运行中收到新的总设定输入会触发一次控制发布，输出点按工程量写入 DataCenter。
TEST(AgcGroupManagerTest, RuntimeCommandUpdateTriggersControlAfterStart) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-runtime-cmd");
  req.mutable_config()->mutable_outputs()->mutable_p_total_meas()->set_scale(2.0);
  req.mutable_config()->mutable_outputs()->mutable_p_total_meas()->set_offset(5.0);
  req.mutable_config()->mutable_members(0)->mutable_p_set()->mutable_signal()->set_scale(2.0);
  req.mutable_config()->mutable_members(0)->mutable_p_set()->mutable_signal()->set_offset(5.0);
  req.mutable_config()->mutable_members(1)->mutable_p_set()->mutable_signal()->set_scale(2.0);
  req.mutable_config()->mutable_members(1)->mutable_p_set()->mutable_signal()->set_offset(5.0);

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  PublishDoublePoint(&state, info.conn_id(), "INV1_P_MEAS", 10.0);
  PublishDoublePoint(&state, info.conn_id(), "INV2_P_MEAS", 20.0);

  ASSERT_TRUE(mgr.StartGroup("g-runtime-cmd").ok());
  ASSERT_TRUE(WaitForSubscriptionCount(state, info.conn_id(), 1u));
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "INV1_P_SET"), 0u);
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "INV2_P_SET"), 0u);

  PublishDoublePoint(&state, info.conn_id(), "P_CMD", 60.0);
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "P_TOTAL", 30.0));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "INV1_P_SET", 20.0));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "INV2_P_SET", 40.0));

  ASSERT_TRUE(mgr.StopGroup("g-runtime-cmd").ok());
}

// 验证：未下发总设定时，成员量测快照与后续量测变化仍会驱动总实时测量值更新，但不会下发成员设定。
TEST(AgcGroupManagerTest, MeasurementUpdatesPublishRealtimeTotalWithoutCommand) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-meas-only-total");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_TRUE(mgr.StopGroup("g-meas-only-total").ok());
  PublishDoublePoint(&state, info.conn_id(), "INV1_P_MEAS", 10.0);
  PublishDoublePoint(&state, info.conn_id(), "INV2_P_MEAS", 20.0);

  ASSERT_TRUE(mgr.StartGroup("g-meas-only-total").ok());
  ASSERT_TRUE(WaitForSubscriptionCount(state, info.conn_id(), 1u));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "P_TOTAL", 30.0));
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "INV1_P_SET"), 0u);
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "INV2_P_SET"), 0u);

  PublishDoublePoint(&state, info.conn_id(), "INV1_P_MEAS", 15.0);
  EXPECT_TRUE(WaitForPublishCount(state, info.conn_id(), "P_TOTAL", 2u));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "P_TOTAL", 35.0));
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "INV1_P_SET"), 0u);
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "INV2_P_SET"), 0u);

  ASSERT_TRUE(mgr.StopGroup("g-meas-only-total").ok());
}

// 验证：控制组运行中收到新的成员量测输入时，会重新触发一次控制计算。
TEST(AgcGroupManagerTest, RuntimeMeasurementUpdateTriggersRecalculation) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-runtime-meas");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  PublishDoublePoint(&state, info.conn_id(), "P_CMD", 60.0);
  PublishDoublePoint(&state, info.conn_id(), "INV1_P_MEAS", 10.0);
  PublishDoublePoint(&state, info.conn_id(), "INV2_P_MEAS", 20.0);

  ASSERT_TRUE(mgr.StartGroup("g-runtime-meas").ok());
  ASSERT_TRUE(WaitForPublishCount(state, info.conn_id(), "P_TOTAL", 1u));
  ASSERT_TRUE(WaitForSubscriptionCount(state, info.conn_id(), 1u));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "P_TOTAL", 30.0));

  PublishDoublePoint(&state, info.conn_id(), "INV1_P_MEAS", 15.0);
  EXPECT_TRUE(WaitForPublishCount(state, info.conn_id(), "P_TOTAL", 2u));
  EXPECT_TRUE(WaitForLatestDouble(state, info.conn_id(), "P_TOTAL", 35.0));

  ASSERT_TRUE(mgr.StopGroup("g-runtime-meas").ok());
}

// 验证：控制组显式启动并完成首轮发布后，运行中收到重复的同值输入时，不会重复触发控制发布。
TEST(AgcGroupManagerTest, DuplicateRuntimeCommandDoesNotRepublish) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-duplicate-cmd");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  ASSERT_TRUE(mgr.StopGroup("g-duplicate-cmd").ok());
  PublishDoublePoint(&state, info.conn_id(), "P_CMD", 60.0);
  PublishDoublePoint(&state, info.conn_id(), "INV1_P_MEAS", 10.0);
  PublishDoublePoint(&state, info.conn_id(), "INV2_P_MEAS", 20.0);

  ASSERT_TRUE(mgr.StartGroup("g-duplicate-cmd").ok());
  ASSERT_TRUE(WaitForPublishCount(state, info.conn_id(), "INV1_P_SET", 1u));
  ASSERT_TRUE(WaitForPublishCount(state, info.conn_id(), "INV2_P_SET", 1u));
  ASSERT_TRUE(WaitForSubscriptionCount(state, info.conn_id(), 1u));

  PublishDoublePoint(&state, info.conn_id(), "P_CMD", 60.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "INV1_P_SET"), 1u);
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "INV2_P_SET"), 1u);
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "P_TOTAL"), 1u);

  ASSERT_TRUE(mgr.StopGroup("g-duplicate-cmd").ok());
}

// 验证：UpsertGroup 会把控制组配置落盘，随后新的 GroupManager 恢复后会自动启动可运行控制组。
TEST(AgcGroupManagerTest, RestorePersistedGroupsLoadsGroupConfigFromLocalStore) {
  ScopedTempDir dir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  const auto configDbPath = dir.path() / "config.db";

  {
    GroupManager writer("AGC", configDbPath);
    writer.setDataCenterStub(stub);

    auto req = MakeGroupReq("g-persist");
    AGCProto::GroupInfo info;
    ASSERT_TRUE(writer.UpsertGroup(req, &info).ok());
    ASSERT_NE(info.conn_id(), 0u);
  }

  GroupManager reader("AGC", configDbPath);
  reader.setDataCenterStub(stub);
  ASSERT_TRUE(reader.LoadPersistedConfig().ok());

  AGCProto::GroupInfo got;
  ASSERT_TRUE(reader.GetGroup("g-persist", &got).ok());
  EXPECT_EQ(got.config().group_name(), "g-persist");
  EXPECT_EQ(got.state(), AGCProto::GROUP_STATE_RUNNING);
  EXPECT_TRUE(got.last_error().empty());
  EXPECT_NE(got.conn_id(), 0u);
  ASSERT_TRUE(reader.StopGroup("g-persist").ok());
}

// 验证：RestorePersistedGroups 恢复出可运行控制组后，会自动启动控制组内功能。
TEST(AgcGroupManagerTest, RestorePersistedGroupsAutoStartsReadyGroup) {
  ScopedTempDir dir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  const auto configDbPath = dir.path() / "config.db";
  uint32_t restoredConnId = 0;

  {
    GroupManager writer("AGC", configDbPath);
    writer.setDataCenterStub(stub);

    auto req = MakeGroupReq("g-auto-restore");
    AGCProto::GroupInfo info;
    ASSERT_TRUE(writer.UpsertGroup(req, &info).ok());
    restoredConnId = info.conn_id();
    ASSERT_NE(restoredConnId, 0u);
  }

  GroupManager reader("AGC", configDbPath);
  reader.setDataCenterStub(stub);
  ASSERT_TRUE(reader.LoadPersistedConfig().ok());

  AGCProto::GroupInfo got;
  ASSERT_TRUE(reader.GetGroup("g-auto-restore", &got).ok());
  EXPECT_EQ(got.state(), AGCProto::GROUP_STATE_RUNNING);
  EXPECT_TRUE(got.last_error().empty());
  EXPECT_EQ(got.conn_id(), restoredConnId);
  EXPECT_TRUE(WaitForSubscriptionCount(state, restoredConnId, 1u));

  ASSERT_TRUE(reader.StopGroup("g-auto-restore").ok());
}

// 验证：DeleteGroup 进入 PENDING_DELETE 后会落盘，重启后仍阻止启动控制组。
TEST(AgcGroupManagerTest, RestorePersistedGroupsLoadsPendingDeleteStateAfterRestart) {
  ScopedTempDir dir;
  FakeDataCenterState state;
  state.FailDeleteForConnName("g-pending-persist");
  auto stub = MakeStub(&state);
  const auto configDbPath = dir.path() / "config.db";

  {
    GroupManager writer("AGC", configDbPath);
    writer.setDataCenterStub(stub);

    auto req = MakeGroupReq("g-pending-persist");
    AGCProto::GroupInfo info;
    ASSERT_TRUE(writer.UpsertGroup(req, &info).ok());

    auto status = writer.DeleteGroup("g-pending-persist");
    ASSERT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  }

  GroupManager reader("AGC", configDbPath);
  reader.setDataCenterStub(stub);
  ASSERT_TRUE(reader.LoadPersistedConfig().ok());

  AGCProto::GroupInfo got;
  ASSERT_TRUE(reader.GetGroup("g-pending-persist", &got).ok());
  EXPECT_EQ(got.state(), AGCProto::GROUP_STATE_PENDING_DELETE);
  EXPECT_TRUE(got.last_error().empty());
  EXPECT_NE(got.conn_id(), 0u);

  auto status = reader.StartGroup("g-pending-persist");
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：DeleteGroup 会同步更新本地落盘文件，重启恢复后不会重新出现已删除控制组。
TEST(AgcGroupManagerTest, DeleteGroupRemovesPersistedConfig) {
  ScopedTempDir dir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  const auto configDbPath = dir.path() / "config.db";

  {
    GroupManager writer("AGC", configDbPath);
    writer.setDataCenterStub(stub);

    auto req = MakeGroupReq("g-removed");
    AGCProto::GroupInfo info;
    ASSERT_TRUE(writer.UpsertGroup(req, &info).ok());
    ASSERT_TRUE(writer.DeleteGroup("g-removed").ok());
  }

  GroupManager reader("AGC", configDbPath);
  reader.setDataCenterStub(stub);
  ASSERT_TRUE(reader.LoadPersistedConfig().ok());

  AGCProto::GroupInfo got;
  auto status = reader.GetGroup("g-removed", &got);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}
