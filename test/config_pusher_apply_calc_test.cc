#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Calc_mock.grpc.pb.h"
#include "ConfigPusherApplyCalc.h"

namespace {
using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Return;

CalcProto::CalcGroupInfo MakeGroupInfo(const char *groupName, CalcProto::GroupState state) {
  CalcProto::CalcGroupInfo info;
  info.mutable_config()->set_group_name(groupName);
  info.set_state(state);
  return info;
}

ConfigPusherProto::CalcConfig MakeCalcConfig(bool start) {
  ConfigPusherProto::CalcConfig config;
  auto *task = config.add_groups();
  auto *upsert = task->mutable_upsert();
  upsert->set_create_only(false);
  auto *group = upsert->mutable_config();
  group->set_group_name("calc-1");
  auto *item = group->add_items();
  item->set_item_name("sum");
  item->set_operator_kind(CalcProto::OPERATOR_KIND_ADD);
  item->mutable_left_operand()->set_source_kind(CalcProto::OPERAND_SOURCE_ROUTED_INPUT);
  item->mutable_right_operand()->set_source_kind(CalcProto::OPERAND_SOURCE_CONSTANT);
  item->mutable_right_operand()->mutable_constant()->set_double_value(10.0);

  task->set_start(start);
  return config;
}
}  // 命名空间结束

// 验证：Calc gRPC stub 为空时下发失败。
TEST(ConfigPusherApplyCalcTest, NullStubReturnsFalse) {
  ConfigPusherProto::CalcConfig config;
  EXPECT_FALSE(ConfigPusher::applyCalcConfig(config, nullptr));
}

// 验证：Calc 配置任务在 start=true 时只调用 UpsertGroup，不再额外调用 StartGroup。
TEST(ConfigPusherApplyCalcTest, UpsertGroupSuccessWhenStartFlagIsTrue) {
  auto config = MakeCalcConfig(true);
  auto stub = std::make_unique<CalcProto::MockCalcServiceStub>();

  InSequence seq;
  EXPECT_CALL(*stub, ListGroups(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const CalcProto::Empty &,
                          CalcProto::ListGroupsResponse *resp) {
        resp->Clear();
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const CalcProto::UpsertGroupRequest &req,
                          CalcProto::CalcGroupInfo *resp) {
        EXPECT_EQ(req.config().group_name(), "calc-1");
        ASSERT_EQ(req.config().items_size(), 1);
        EXPECT_EQ(req.config().items(0).operator_kind(), CalcProto::OPERATOR_KIND_ADD);
        EXPECT_EQ(req.config().items(0).right_operand().source_kind(), CalcProto::OPERAND_SOURCE_CONSTANT);
        resp->mutable_config()->set_group_name(req.config().group_name());
        resp->set_conn_id(300);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyCalcConfig(config, stub.get()));
}

// 验证：UpsertGroup 失败时不会继续调用 StartGroup，并返回失败。
TEST(ConfigPusherApplyCalcTest, UpsertFailureSkipsStartGroup) {
  auto config = MakeCalcConfig(true);
  auto stub = std::make_unique<CalcProto::MockCalcServiceStub>();

  EXPECT_CALL(*stub, UpsertGroup(_, _, _))
      .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "配置失败")));
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyCalcConfig(config, stub.get()));
}

// 验证：Calc 配置任务在 start=false 时仅下发 UpsertGroup，不会启动分组运算功能。
TEST(ConfigPusherApplyCalcTest, UpsertWithoutStartGroup) {
  auto config = MakeCalcConfig(false);
  auto stub = std::make_unique<CalcProto::MockCalcServiceStub>();

  EXPECT_CALL(*stub, UpsertGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const CalcProto::UpsertGroupRequest &req,
                          CalcProto::CalcGroupInfo *resp) {
        EXPECT_EQ(req.config().group_name(), "calc-1");
        resp->mutable_config()->set_group_name(req.config().group_name());
        resp->set_conn_id(301);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyCalcConfig(config, stub.get()));
}

// 验证：缺少 config.group_name 时不会继续调用 UpsertGroup，并返回失败。
TEST(ConfigPusherApplyCalcTest, MissingGroupNameSkipsRpc) {
  auto config = MakeCalcConfig(false);
  config.mutable_groups(0)->mutable_upsert()->mutable_config()->set_group_name("");
  auto stub = std::make_unique<CalcProto::MockCalcServiceStub>();

  EXPECT_CALL(*stub, UpsertGroup(_, _, _)).Times(0);
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyCalcConfig(config, stub.get()));
}

// 验证：CONFIG_PUSHER 编排会删除 jsonc 未声明的旧计算分组，并先停止仍在运行的目标分组再按 jsonc 覆盖配置。
TEST(ConfigPusherApplyCalcTest, ReconcilesStaleAndRunningGroupsToJsoncTargetState) {
  auto config = MakeCalcConfig(true);
  auto stub = std::make_unique<CalcProto::MockCalcServiceStub>();

  InSequence seq;
  EXPECT_CALL(*stub, ListGroups(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const CalcProto::Empty &,
                          CalcProto::ListGroupsResponse *resp) {
        resp->Clear();
        *resp->add_groups() = MakeGroupInfo("calc-legacy", CalcProto::GROUP_STATE_STOPPED);
        *resp->add_groups() = MakeGroupInfo("calc-1", CalcProto::GROUP_STATE_RUNNING);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, DeleteGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const CalcProto::DeleteGroupRequest &req,
                          CalcProto::Empty *) {
        EXPECT_EQ(req.group_name(), "calc-legacy");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StopGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const CalcProto::StopGroupRequest &req,
                          CalcProto::Empty *) {
        EXPECT_EQ(req.group_name(), "calc-1");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertGroup(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const CalcProto::UpsertGroupRequest &req,
                          CalcProto::CalcGroupInfo *resp) {
        EXPECT_EQ(req.config().group_name(), "calc-1");
        ASSERT_EQ(req.config().items_size(), 1);
        EXPECT_EQ(req.config().items(0).item_name(), "sum");
        resp->mutable_config()->set_group_name(req.config().group_name());
        resp->set_conn_id(302);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartGroup(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyCalcConfig(config, stub.get()));
}
