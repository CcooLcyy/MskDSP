#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
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
        std::chrono::system_clock::now().time_since_epoch()).count();
    path_ = base / ("agc_group_manager_test_tmp_" + std::to_string(ts));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

AGCProto::UpsertGroupRequest MakeGroupReq(const char* groupName) {
  AGCProto::UpsertGroupRequest req;
  req.set_create_only(true);
  auto* cfg = req.mutable_config();
  cfg->set_group_name(groupName);
  cfg->mutable_p_cmd()->mutable_signal()->set_tag("P_CMD");
  cfg->mutable_p_cmd()->mutable_signal()->set_unit("kW");
  cfg->mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  cfg->mutable_outputs()->mutable_p_total_meas()->set_tag("P_TOTAL");
  cfg->mutable_outputs()->mutable_p_total_meas()->set_unit("kW");

  auto* inv1 = cfg->add_members();
  inv1->set_member_name("inv-1");
  inv1->set_controllable(true);
  inv1->set_capacity_kw(50);
  inv1->set_weight(50);
  inv1->mutable_p_meas()->set_tag("INV1_P_MEAS");
  inv1->mutable_p_meas()->set_unit("kW");
  inv1->mutable_p_set()->mutable_signal()->set_tag("INV1_P_SET");
  inv1->mutable_p_set()->mutable_signal()->set_unit("kW");
  inv1->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  auto* inv2 = cfg->add_members();
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
}  // namespace

// 验证：create_only UpsertGroup 会向 DataCenter 取/建 conn_id，并回填到 GroupInfo。
TEST(AgcGroupManagerTest, UpsertGroupCreateOnlyReturnsConnId) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  GroupManager mgr("AGC");
  mgr.setDataCenterStub(stub);
  auto req = MakeGroupReq("g-1");

  AGCProto::GroupInfo info;
  ASSERT_TRUE(mgr.UpsertGroup(req, &info).ok());
  EXPECT_NE(info.conn_id(), 0u);
  EXPECT_EQ(info.state(), AGCProto::GROUP_STATE_STOPPED);
  EXPECT_EQ(info.config().group_name(), "g-1");
  EXPECT_TRUE(state.HasConnection("AGC", "g-1"));
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
      .WillOnce(Invoke([](grpc::ClientContext*, const DataCenterProto::UpsertConnTagsRequest& req, DataCenterProto::Empty*) {
        EXPECT_NE(req.conn_id(), 0u);
        EXPECT_TRUE(req.replace());

        std::unordered_set<std::string> tags;
        for (const auto& t : req.tags()) {
          tags.emplace(t);
        }
        EXPECT_TRUE(tags.contains("P_CMD"));
        EXPECT_TRUE(tags.contains("P_BASE"));
        EXPECT_TRUE(tags.contains("INV1_P_MEAS"));
        EXPECT_TRUE(tags.contains("INV1_P_SET"));
        EXPECT_TRUE(tags.contains("INV2_P_MEAS"));
        EXPECT_TRUE(tags.contains("INV2_P_SET"));
        EXPECT_TRUE(tags.contains("P_TOTAL"));
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

// 验证：UpsertGroup 会把控制组配置落盘，随后可由新的 GroupManager 恢复为 STOPPED 状态。
TEST(AgcGroupManagerTest, RestorePersistedGroupsLoadsStoppedGroupsFromLocalStore) {
  ScopedTempDir dir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  const auto groupsPath = dir.path() / "groups.pb";

  {
    GroupManager writer("AGC", groupsPath);
    writer.setDataCenterStub(stub);

    auto req = MakeGroupReq("g-persist");
    AGCProto::GroupInfo info;
    ASSERT_TRUE(writer.UpsertGroup(req, &info).ok());
    ASSERT_NE(info.conn_id(), 0u);
  }

  GroupManager reader("AGC", groupsPath);
  reader.setDataCenterStub(stub);
  ASSERT_TRUE(reader.RestorePersistedGroups().ok());

  AGCProto::GroupInfo got;
  ASSERT_TRUE(reader.GetGroup("g-persist", &got).ok());
  EXPECT_EQ(got.config().group_name(), "g-persist");
  EXPECT_EQ(got.state(), AGCProto::GROUP_STATE_STOPPED);
  EXPECT_TRUE(got.last_error().empty());
  EXPECT_NE(got.conn_id(), 0u);
}

// 验证：DeleteGroup 进入 PENDING_DELETE 后会落盘，重启后仍阻止启动控制组。
TEST(AgcGroupManagerTest, RestorePersistedGroupsLoadsPendingDeleteStateAfterRestart) {
  ScopedTempDir dir;
  FakeDataCenterState state;
  state.FailDeleteForConnName("g-pending-persist");
  auto stub = MakeStub(&state);
  const auto groupsPath = dir.path() / "groups.pb";

  {
    GroupManager writer("AGC", groupsPath);
    writer.setDataCenterStub(stub);

    auto req = MakeGroupReq("g-pending-persist");
    AGCProto::GroupInfo info;
    ASSERT_TRUE(writer.UpsertGroup(req, &info).ok());

    auto status = writer.DeleteGroup("g-pending-persist");
    ASSERT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  }

  GroupManager reader("AGC", groupsPath);
  reader.setDataCenterStub(stub);
  ASSERT_TRUE(reader.RestorePersistedGroups().ok());

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
  const auto groupsPath = dir.path() / "groups.pb";

  {
    GroupManager writer("AGC", groupsPath);
    writer.setDataCenterStub(stub);

    auto req = MakeGroupReq("g-removed");
    AGCProto::GroupInfo info;
    ASSERT_TRUE(writer.UpsertGroup(req, &info).ok());
    ASSERT_TRUE(writer.DeleteGroup("g-removed").ok());
  }

  GroupManager reader("AGC", groupsPath);
  reader.setDataCenterStub(stub);
  ASSERT_TRUE(reader.RestorePersistedGroups().ok());

  AGCProto::GroupInfo got;
  auto status = reader.GetGroup("g-removed", &got);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}
