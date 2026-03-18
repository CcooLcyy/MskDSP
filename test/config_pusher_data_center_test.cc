#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <unordered_set>

#include "ConfigPusherDataCenter.h"
#include "DataCenter_mock.grpc.pb.h"
#include "Logger.h"
#include "support/FakeDataCenter.hpp"

namespace {
using ::testing::_;
using ::testing::Invoke;

ConfigPusherProto::DataCenterConfig MakeConfig(const char* srcModule,
                                               const char* srcConn,
                                               const char* dstModule,
                                               const char* dstConn) {
  ConfigPusherProto::DataCenterConfig config;
  auto* pt = config.add_point_tables();
  pt->set_module_name(srcModule);
  pt->set_conn_name(srcConn);
  pt->set_replace(true);
  pt->add_tags("P_CMD_SRC");
  pt->add_tags("P_TOTAL_DST");

  auto* routes = config.mutable_routes();
  routes->set_replace(true);
  auto* route = routes->add_routes();
  route->mutable_src()->set_module_name(srcModule);
  route->mutable_src()->set_conn_name(srcConn);
  route->mutable_src()->set_tag("P_CMD_SRC");
  route->mutable_dst()->set_module_name(dstModule);
  route->mutable_dst()->set_conn_name(dstConn);
  route->mutable_dst()->set_tag("P_CMD");
  return config;
}
}  // 命名空间结束

// 验证：当路由引用不存在连接时，DataCenter 配置不会继续下发。
TEST(ConfigPusherDataCenterTest, AbortWhenConnectionMissing) {
  FakeDataCenterState state;
  state.AddConnection(10, "IEC104", "line-1");
  auto stub = MakeStub(&state);

  EXPECT_CALL(*stub, UpsertConnTags(_, _, _)).Times(0);
  EXPECT_CALL(*stub, UpsertRoutes(_, _, _)).Times(0);

  auto config = MakeConfig("IEC104", "line-1", "AGC", "g-1");
  EXPECT_FALSE(ConfigPusher::ApplyDataCenterConfig(config, stub.get()));
}

// 验证：连接存在时可下发连接标签注册表与路由。
TEST(ConfigPusherDataCenterTest, ApplyPointTablesAndRoutes) {
  FakeDataCenterState state;
  state.AddConnection(10, "IEC104", "line-1");
  state.AddConnection(20, "AGC", "g-1");
  auto stub = MakeStub(&state);

  EXPECT_CALL(*stub, UpsertConnTags(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*, const DataCenterProto::UpsertConnTagsRequest& req, DataCenterProto::Empty*) {
        EXPECT_EQ(req.conn_id(), 10u);
        EXPECT_TRUE(req.replace());
        std::unordered_set<std::string> tags;
        for (const auto& tag : req.tags()) {
          tags.emplace(tag);
        }
        EXPECT_TRUE(tags.contains("P_CMD_SRC"));
        EXPECT_TRUE(tags.contains("P_TOTAL_DST"));
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, UpsertRoutes(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*, const DataCenterProto::UpsertRoutesRequest& req, DataCenterProto::Empty*) {
        EXPECT_TRUE(req.replace());
        if (req.routes_size() != 1) {
          LOG_ERROR("UpsertRoutes expected 1 route, got {}", req.routes_size());
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "unexpected routes_size");
        }
        const auto& route = req.routes(0);
        EXPECT_EQ(route.src().conn_id(), 10u);
        EXPECT_EQ(route.src().tag(), "P_CMD_SRC");
        EXPECT_EQ(route.dst().conn_id(), 20u);
        EXPECT_EQ(route.dst().tag(), "P_CMD");
        return grpc::Status::OK;
      }));

  auto config = MakeConfig("IEC104", "line-1", "AGC", "g-1");
  EXPECT_TRUE(ConfigPusher::ApplyDataCenterConfig(config, stub.get()));
}
