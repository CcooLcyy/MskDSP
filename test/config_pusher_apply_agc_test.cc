#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "AGC_mock.grpc.pb.h"
#include "ConfigPusherApplyAgc.h"

namespace {
using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Return;

AGCProto::GroupInfo MakeGroupInfo(const char *groupName, AGCProto::GroupState state) {
  AGCProto::GroupInfo info;
  info.mutable_config()->set_group_name(groupName);
  info.set_state(state);
  return info;
}

ConfigPusherProto::AgcConfig MakeAgcConfig(bool start) {
  ConfigPusherProto::AgcConfig config;
  auto* task = config.add_groups();
  auto* upsert = task->mutable_upsert();
  upsert->set_create_only(false);
  auto* group = upsert->mutable_config();
  group->set_group_name("g-1");
  group->mutable_p_cmd()->mutable_signal()->set_tag("P_CMD");
  group->mutable_p_cmd()->mutable_signal()->set_unit("kW");
  group->mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);
  group->mutable_strategy()->mutable_weighted();

  auto* member = group->add_members();
  member->set_member_name("inv-1");
  member->set_controllable(true);
  member->mutable_p_meas()->set_tag("INV1_P_MEAS");
  member->mutable_p_meas()->set_unit("kW");
  member->mutable_p_set()->mutable_signal()->set_tag("INV1_P_SET");
  member->mutable_p_set()->mutable_signal()->set_unit("kW");
  member->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  task->set_start(start);
  return config;
}
}  // 命名空间结束

// 验证：AGC gRPC stub 为空时下发失败。
TEST(ConfigPusherApplyAgcTest, NullStubReturnsFalse) {
  ConfigPusherProto::AgcConfig config;
  EXPECT_FALSE(ConfigPusher::applyAgcConfig(config, nullptr));
}

// 验证：AGC 配置任务在 start=true 时只调用 UpsertGroup，不再额外调用 StartGroup。
TEST(ConfigPusherApplyAgcTest, UpsertGroupSuccessWhenStartFlagIsTrue) {
  auto config = MakeAgcConfig(true);
  auto stub = std::make_unique<AGCProto::MockAGCServiceStub>();

  InSequence seq;
  EXPECT_CALL(*stub, ListGroups(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const AGCProto::Empty &,
                          AGCProto::ListGroupsResponse *resp) {
        resp->Clear();
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const AGCProto::UpsertGroupRequest& req,
                          AGCProto::GroupInfo* resp) {
        EXPECT_EQ(req.config().group_name(), "g-1");
        EXPECT_TRUE(req.config().has_strategy());
        EXPECT_TRUE(req.config().strategy().has_weighted());
        EXPECT_EQ(req.config().members_size(), 1);
        resp->mutable_config()->set_group_name(req.config().group_name());
        resp->set_conn_id(100);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyAgcConfig(config, stub.get()));
}

// 验证：UpsertGroup 失败时不会继续调用 StartGroup，并返回失败。
TEST(ConfigPusherApplyAgcTest, UpsertFailureSkipsStartGroup) {
  auto config = MakeAgcConfig(true);
  auto stub = std::make_unique<AGCProto::MockAGCServiceStub>();

  EXPECT_CALL(*stub, UpsertGroup(_, _, _))
      .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "配置失败")));
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyAgcConfig(config, stub.get()));
}

// 验证：AGC 配置任务在 start=false 时仅下发 UpsertGroup，不会启动控制组功能。
TEST(ConfigPusherApplyAgcTest, UpsertWithoutStartGroup) {
  auto config = MakeAgcConfig(false);
  auto stub = std::make_unique<AGCProto::MockAGCServiceStub>();

  EXPECT_CALL(*stub, UpsertGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const AGCProto::UpsertGroupRequest& req,
                          AGCProto::GroupInfo* resp) {
        EXPECT_EQ(req.config().group_name(), "g-1");
        resp->mutable_config()->set_group_name(req.config().group_name());
        resp->set_conn_id(101);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyAgcConfig(config, stub.get()));
}

// 验证：缺少 config.group_name 时不会继续调用 UpsertGroup，并返回失败。
TEST(ConfigPusherApplyAgcTest, MissingGroupNameSkipsRpc) {
  auto config = MakeAgcConfig(false);
  config.mutable_groups(0)->mutable_upsert()->mutable_config()->set_group_name("");
  auto stub = std::make_unique<AGCProto::MockAGCServiceStub>();

  EXPECT_CALL(*stub, UpsertGroup(_, _, _)).Times(0);
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyAgcConfig(config, stub.get()));
}

// 验证：CONFIG_PUSHER 编排会删除 jsonc 未声明的旧控制组，并先停止仍在运行的目标控制组再按 jsonc 覆盖配置。
TEST(ConfigPusherApplyAgcTest, ReconcilesStaleAndRunningGroupsToJsoncTargetState) {
  auto config = MakeAgcConfig(true);
  auto stub = std::make_unique<AGCProto::MockAGCServiceStub>();

  InSequence seq;
  EXPECT_CALL(*stub, ListGroups(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const AGCProto::Empty &,
                          AGCProto::ListGroupsResponse *resp) {
        resp->Clear();
        *resp->add_groups() = MakeGroupInfo("g-legacy", AGCProto::GROUP_STATE_STOPPED);
        *resp->add_groups() = MakeGroupInfo("g-1", AGCProto::GROUP_STATE_RUNNING);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, DeleteGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const AGCProto::DeleteGroupRequest &req,
                          AGCProto::Empty *) {
        EXPECT_EQ(req.group_name(), "g-legacy");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StopGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const AGCProto::StopGroupRequest &req,
                          AGCProto::Empty *) {
        EXPECT_EQ(req.group_name(), "g-1");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const AGCProto::UpsertGroupRequest& req,
                          AGCProto::GroupInfo* resp) {
        EXPECT_EQ(req.config().group_name(), "g-1");
        EXPECT_EQ(req.config().members_size(), 1);
        resp->mutable_config()->set_group_name(req.config().group_name());
        resp->set_conn_id(102);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyAgcConfig(config, stub.get()));
}
