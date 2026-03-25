#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include "ConfigPusherApplyIec104.h"
#include "IEC104_mock.grpc.pb.h"

namespace {
using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;

IEC104Proto::LinkInfo MakeLinkInfo(const char *connName, IEC104Proto::LinkState state) {
  IEC104Proto::LinkInfo info;
  info.mutable_config()->set_conn_name(connName);
  info.set_state(state);
  return info;
}

ConfigPusherProto::Iec104Config MakeIec104Config(bool start) {
  ConfigPusherProto::Iec104Config config;

  auto *task = config.add_links();
  auto *linkCfg = task->mutable_link()->mutable_config();
  linkCfg->set_conn_name("iec104-main");
  linkCfg->set_role(IEC104Proto::ROLE_CLIENT);
  linkCfg->mutable_remote()->set_ip("127.0.0.1");
  linkCfg->mutable_remote()->set_port(2404);
  linkCfg->set_ca(1);
  linkCfg->set_oa(1);

  auto *point = task->mutable_point_table()->add_points();
  point->set_tag("P_TOTAL");
  point->set_ioa(1001);
  point->set_type(IEC104Proto::POINT_TYPE_FLOAT);

  task->set_start(start);
  return config;
}
}  // 命名空间结束

// 验证：IEC104 gRPC stub 为空时下发失败。
TEST(ConfigPusherApplyIec104Test, NullStubReturnsFalse) {
  ConfigPusherProto::Iec104Config config;
  EXPECT_FALSE(ConfigPusher::applyIec104Config(config, nullptr));
}

// 验证：IEC104 配置任务在 start=true 时只下发连接和点表，不再额外调用 StartLink。
TEST(ConfigPusherApplyIec104Test, ApplyLinkAndPointTableWithoutExplicitStart) {
  auto config = MakeIec104Config(true);
  auto stub = std::make_unique<IEC104Proto::MockIEC104ServiceStub>();

  InSequence seq;
  EXPECT_CALL(*stub, ListLinks(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const IEC104Proto::Empty &,
                          IEC104Proto::ListLinksResponse *resp) {
        resp->Clear();
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertLink(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const IEC104Proto::UpsertLinkRequest &req,
                          IEC104Proto::LinkInfo *resp) {
        EXPECT_EQ(req.config().conn_name(), "iec104-main");
        EXPECT_EQ(req.config().role(), IEC104Proto::ROLE_CLIENT);
        resp->set_conn_id(301);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertPointTable(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const IEC104Proto::UpsertPointTableRequest &req,
                          IEC104Proto::Empty *) {
        EXPECT_EQ(req.conn_name(), "iec104-main");
        EXPECT_EQ(req.points_size(), 1);
        EXPECT_EQ(req.points(0).tag(), "P_TOTAL");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartLink(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyIec104Config(config, stub.get()));
}

// 验证：缺少 config.conn_name 时不会继续下发连接配置，并返回失败。
TEST(ConfigPusherApplyIec104Test, MissingConnNameSkipsRpc) {
  auto config = MakeIec104Config(false);
  config.mutable_links(0)->mutable_link()->mutable_config()->set_conn_name("");
  auto stub = std::make_unique<IEC104Proto::MockIEC104ServiceStub>();

  EXPECT_CALL(*stub, UpsertLink(_, _, _)).Times(0);
  EXPECT_CALL(*stub, UpsertPointTable(_, _, _)).Times(0);
  EXPECT_CALL(*stub, StartLink(_, _, _)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyIec104Config(config, stub.get()));
}

// 验证：CONFIG_PUSHER 编排会删除 jsonc 未声明的旧链路、先停止仍在运行的目标链路，再按全量点表覆盖到 jsonc 目标态。
TEST(ConfigPusherApplyIec104Test, ReconcilesStaleAndRunningLinksToJsoncTargetState) {
  auto config = MakeIec104Config(true);
  config.mutable_links(0)->mutable_point_table()->set_replace(false);
  auto stub = std::make_unique<IEC104Proto::MockIEC104ServiceStub>();

  InSequence seq;
  EXPECT_CALL(*stub, ListLinks(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const IEC104Proto::Empty &,
                          IEC104Proto::ListLinksResponse *resp) {
        resp->Clear();
        *resp->add_links() = MakeLinkInfo("iec104-legacy", IEC104Proto::LINK_STATE_STOPPED);
        *resp->add_links() = MakeLinkInfo("iec104-main", IEC104Proto::LINK_STATE_RUNNING);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, DeleteLink(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const IEC104Proto::DeleteLinkRequest &req,
                          IEC104Proto::Empty *) {
        EXPECT_EQ(req.conn_name(), "iec104-legacy");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StopLink(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const IEC104Proto::StopLinkRequest &req,
                          IEC104Proto::Empty *) {
        EXPECT_EQ(req.conn_name(), "iec104-main");
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertLink(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const IEC104Proto::UpsertLinkRequest &req,
                          IEC104Proto::LinkInfo *resp) {
        EXPECT_EQ(req.config().conn_name(), "iec104-main");
        resp->set_conn_id(302);
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, UpsertPointTable(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const IEC104Proto::UpsertPointTableRequest &req,
                          IEC104Proto::Empty *) {
        EXPECT_EQ(req.conn_name(), "iec104-main");
        EXPECT_TRUE(req.replace());
        EXPECT_EQ(req.points_size(), 1);
        if (req.points_size() == 1) {
          EXPECT_EQ(req.points(0).tag(), "P_TOTAL");
        }
        return grpc::Status::OK;
      }));
  EXPECT_CALL(*stub, StartLink(_, _, _)).Times(0);

  EXPECT_TRUE(ConfigPusher::applyIec104Config(config, stub.get()));
}
