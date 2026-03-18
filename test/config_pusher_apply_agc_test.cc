#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "AGC_mock.grpc.pb.h"
#include "ConfigPusherApplyAgc.h"

namespace {
using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

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

// 验证：AGC 配置任务会调用 UpsertGroup，并在 start=true 时调用 StartGroup。
TEST(ConfigPusherApplyAgcTest, UpsertAndStartGroupSuccess) {
  auto config = MakeAgcConfig(true);
  auto stub = std::make_unique<AGCProto::MockAGCServiceStub>();

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
  EXPECT_CALL(*stub, StartGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const AGCProto::StartGroupRequest& req,
                          AGCProto::Empty*) {
        EXPECT_EQ(req.group_name(), "g-1");
        return grpc::Status::OK;
      }));

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
