#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "AVC_mock.grpc.pb.h"
#include "ConfigPusherApplyAvc.h"

namespace {
using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Return;

AVCProto::GroupInfo MakeGroupInfo(const char *groupName, AVCProto::GroupState state) {
  AVCProto::GroupInfo info;
  info.mutable_config()->set_group_name(groupName);
  info.set_state(state);
  return info;
}

ConfigPusherProto::AvcConfig MakeAvcConfig(bool start) {
  ConfigPusherProto::AvcConfig config;
  auto *task = config.add_groups();
  auto *upsert = task->mutable_upsert();
  upsert->set_create_only(false);
  auto *group = upsert->mutable_config();
  group->set_group_name("avc-1");
  group->mutable_voltage_meas()->set_tag("BUS_V_MEAS");
  group->mutable_voltage_meas()->set_unit("V");
  group->mutable_voltage_cmd()->set_tag("BUS_V_CMD");
  group->mutable_voltage_cmd()->set_unit("V");
  group->mutable_voltage_control()->set_kp(1.8);
  group->mutable_voltage_control()->set_deadband(0.1);
  group->mutable_strategy()->mutable_weighted();

  auto *member = group->add_members();
  member->set_member_name("svg-1");
  member->set_controllable(true);
  member->set_weight(1.5);
  member->set_q_min_kvar(-200);
  member->set_q_max_kvar(200);
  member->mutable_q_meas()->set_tag("SVG1_Q_MEAS");
  member->mutable_q_meas()->set_unit("kVar");
  member->mutable_q_set()->mutable_signal()->set_tag("SVG1_Q_SET");
  member->mutable_q_set()->mutable_signal()->set_unit("kVar");
  member->mutable_q_set()->set_mode(AVCProto::VALUE_MODE_ABSOLUTE);

  task->set_start(start);
  return config;
}
}  // 命名空间结束

// 验证：AVC gRPC stub 为空时下发失败。
TEST(ConfigPusherApplyAvcTest, NullStubReturnsFalse) {
  ConfigPusherProto::AvcConfig config;
  EXPECT_FALSE(ConfigPusher::applyAvcConfig(config, nullptr));
}

// 验证：AVC 配置任务在 start=true 时只调用 UpsertGroup，不再额外调用 StartGroup。
TEST(ConfigPusherApplyAvcTest, UpsertGroupSuccessWhenStartFlagIsTrue) {
  auto config = MakeAvcConfig(true);
  auto stub = std::make_unique<AVCProto::MockAVCServiceStub>();

  InSequence seq;
  EXPECT_CALL(*stub, ListGroups(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const AVCProto::Empty &,
                          AVCProto::ListGroupsResponse *resp) {
        resp->Clear();
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const AVCProto::UpsertGroupRequest &req,
                          AVCProto::GroupInfo *resp) {
        EXPECT_EQ(req.config().group_name(), "avc-1");
        EXPECT_TRUE(req.config().has_voltage_meas());
        EXPECT_TRUE(req.config().has_voltage_cmd());
        EXPECT_EQ(req.config().members_size(), 1);
        resp->mutable_config()->set_group_name(req.config().group_name());
        resp->set_conn_id(200);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyAvcConfig(config, stub.get()));
}

// 验证：UpsertGroup 失败时不会继续调用 StartGroup，并返回失败。
TEST(ConfigPusherApplyAvcTest, UpsertFailureSkipsStartGroup) {
  auto config = MakeAvcConfig(true);
  auto stub = std::make_unique<AVCProto::MockAVCServiceStub>();

  EXPECT_CALL(*stub, UpsertGroup(_, _, _))
      .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "配置失败")));
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyAvcConfig(config, stub.get()));
}

// 验证：AVC 配置任务在 start=false 时仅下发 UpsertGroup，不会启动控制组功能。
TEST(ConfigPusherApplyAvcTest, UpsertWithoutStartGroup) {
  auto config = MakeAvcConfig(false);
  auto stub = std::make_unique<AVCProto::MockAVCServiceStub>();

  EXPECT_CALL(*stub, UpsertGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const AVCProto::UpsertGroupRequest &req,
                          AVCProto::GroupInfo *resp) {
        EXPECT_EQ(req.config().group_name(), "avc-1");
        resp->mutable_config()->set_group_name(req.config().group_name());
        resp->set_conn_id(201);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyAvcConfig(config, stub.get()));
}

// 验证：缺少 config.group_name 时不会继续调用 UpsertGroup，并返回失败。
TEST(ConfigPusherApplyAvcTest, MissingGroupNameSkipsRpc) {
  auto config = MakeAvcConfig(false);
  config.mutable_groups(0)->mutable_upsert()->mutable_config()->set_group_name("");
  auto stub = std::make_unique<AVCProto::MockAVCServiceStub>();

  EXPECT_CALL(*stub, UpsertGroup(_, _, _)).Times(0);
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyAvcConfig(config, stub.get()));
}

// 验证：CONFIG_PUSHER 编排会删除 jsonc 未声明的旧控制组，并先停止仍在运行的目标控制组再按 jsonc 覆盖配置。
TEST(ConfigPusherApplyAvcTest, ReconcilesStaleAndRunningGroupsToJsoncTargetState) {
  auto config = MakeAvcConfig(true);
  auto stub = std::make_unique<AVCProto::MockAVCServiceStub>();

  InSequence seq;
  EXPECT_CALL(*stub, ListGroups(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const AVCProto::Empty &,
                          AVCProto::ListGroupsResponse *resp) {
        resp->Clear();
        *resp->add_groups() = MakeGroupInfo("avc-legacy", AVCProto::GROUP_STATE_STOPPED);
        *resp->add_groups() = MakeGroupInfo("avc-1", AVCProto::GROUP_STATE_RUNNING);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, DeleteGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const AVCProto::DeleteGroupRequest &req,
                          AVCProto::Empty *) {
        EXPECT_EQ(req.group_name(), "avc-legacy");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StopGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const AVCProto::StopGroupRequest &req,
                          AVCProto::Empty *) {
        EXPECT_EQ(req.group_name(), "avc-1");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const AVCProto::UpsertGroupRequest &req,
                          AVCProto::GroupInfo *resp) {
        EXPECT_EQ(req.config().group_name(), "avc-1");
        EXPECT_EQ(req.config().members_size(), 1);
        resp->mutable_config()->set_group_name(req.config().group_name());
        resp->set_conn_id(202);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyAvcConfig(config, stub.get()));
}
