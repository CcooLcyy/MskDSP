#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ConfigPusherDataCenter.h"
#include "DataCenter_mock.grpc.pb.h"
#include "Logger.h"
#include "support/FakeDataCenter.hpp"

namespace {
using ::testing::_;
using ::testing::AnyNumber;
using ::testing::AtLeast;
using ::testing::Invoke;
using ::testing::UnorderedElementsAre;

ConfigPusherProto::DataCenterConfig MakeConfig(const char* srcModule,
                                               const char* srcConn,
                                               const char* dstModule,
                                               const char* dstConn) {
  ConfigPusherProto::DataCenterConfig config;
  auto* srcPt = config.add_point_tables();
  srcPt->set_module_name(srcModule);
  srcPt->set_conn_name(srcConn);
  srcPt->set_replace(true);
  srcPt->add_tags("P_CMD_SRC");
  srcPt->add_tags("P_TOTAL_DST");

  auto* dstPt = config.add_point_tables();
  dstPt->set_module_name(dstModule);
  dstPt->set_conn_name(dstConn);
  dstPt->set_replace(true);
  dstPt->add_tags("P_CMD");

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

DataCenterProto::ConnectionInfo MakeConnection(uint32_t connId,
                                               const char* moduleName,
                                               const char* connName) {
  DataCenterProto::ConnectionInfo conn;
  conn.set_conn_id(connId);
  conn.set_module_name(moduleName);
  conn.set_conn_name(connName);
  return conn;
}

DataCenterProto::Route MakeResolvedRoute(uint32_t srcConnId,
                                         const char* srcModule,
                                         const char* srcConn,
                                         const char* srcTag,
                                         uint32_t dstConnId,
                                         const char* dstModule,
                                         const char* dstConn,
                                         const char* dstTag) {
  DataCenterProto::Route route;
  route.mutable_src()->set_conn_id(srcConnId);
  route.mutable_src()->set_module_name(srcModule);
  route.mutable_src()->set_conn_name(srcConn);
  route.mutable_src()->set_tag(srcTag);
  route.mutable_dst()->set_conn_id(dstConnId);
  route.mutable_dst()->set_module_name(dstModule);
  route.mutable_dst()->set_conn_name(dstConn);
  route.mutable_dst()->set_tag(dstTag);
  return route;
}

bool RouteEquals(const DataCenterProto::Route& lhs, const DataCenterProto::Route& rhs) {
  return lhs.src().conn_id() == rhs.src().conn_id() && lhs.src().tag() == rhs.src().tag() &&
         lhs.src().module_name() == rhs.src().module_name() && lhs.src().conn_name() == rhs.src().conn_name() &&
         lhs.dst().conn_id() == rhs.dst().conn_id() && lhs.dst().tag() == rhs.dst().tag() &&
         lhs.dst().module_name() == rhs.dst().module_name() && lhs.dst().conn_name() == rhs.dst().conn_name();
}

bool RouteMatchesRequest(const DataCenterProto::Route& route, const DataCenterProto::ListRoutesRequest& req) {
  if (req.src_conn_id() != 0 && route.src().conn_id() != req.src_conn_id()) {
    return false;
  }
  if (!req.src_tag().empty() && route.src().tag() != req.src_tag()) {
    return false;
  }
  if (req.dst_conn_id() != 0 && route.dst().conn_id() != req.dst_conn_id()) {
    return false;
  }
  if (!req.dst_tag().empty() && route.dst().tag() != req.dst_tag()) {
    return false;
  }
  return true;
}

std::vector<std::string> ToTagVector(const google::protobuf::RepeatedPtrField<std::string>& tags) {
  return {tags.begin(), tags.end()};
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

// 验证：路由 tag 未在目标连接标签注册表中声明时，不执行任何写入。
TEST(ConfigPusherDataCenterTest, AbortBeforeWritesWhenRouteTagMissingFromConnTags) {
  FakeDataCenterState state;
  state.AddConnection(10, "IEC104", "line-1");
  state.AddConnection(20, "AGC", "g-1");
  auto stub = MakeStub(&state);

  EXPECT_CALL(*stub, UpsertConnTags(_, _, _)).Times(0);
  EXPECT_CALL(*stub, UpsertRoutes(_, _, _)).Times(0);

  auto config = MakeConfig("IEC104", "line-1", "AGC", "g-1");
  config.mutable_routes()->mutable_routes(0)->mutable_src()->set_tag("未声明点");
  EXPECT_FALSE(ConfigPusher::ApplyDataCenterConfig(config, stub.get()));
}

// 验证：连接存在时可下发连接标签注册表与路由。
TEST(ConfigPusherDataCenterTest, ApplyPointTablesAndRoutes) {
  FakeDataCenterState state;
  state.AddConnection(10, "IEC104", "line-1");
  state.AddConnection(20, "AGC", "g-1");
  auto stub = MakeStub(&state);

  int connTagsCallCount = 0;
  EXPECT_CALL(*stub, UpsertConnTags(_, _, _))
      .Times(2)
      .WillRepeatedly(Invoke([&](grpc::ClientContext*, const DataCenterProto::UpsertConnTagsRequest& req, DataCenterProto::Empty*) {
        EXPECT_TRUE(req.replace());
        std::unordered_set<std::string> tags;
        for (const auto& tag : req.tags()) {
          tags.emplace(tag);
        }
        ++connTagsCallCount;
        if (req.conn_id() == 10u) {
          EXPECT_TRUE(tags.contains("P_CMD_SRC"));
          EXPECT_TRUE(tags.contains("P_TOTAL_DST"));
        } else if (req.conn_id() == 20u) {
          EXPECT_EQ(tags.size(), 1u);
          EXPECT_TRUE(tags.contains("P_CMD"));
        } else {
          ADD_FAILURE() << "UpsertConnTags 命中了未知连接: " << req.conn_id();
        }
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
        EXPECT_EQ(route.src().module_name(), "IEC104");
        EXPECT_EQ(route.src().conn_name(), "line-1");
        EXPECT_EQ(route.src().tag(), "P_CMD_SRC");
        EXPECT_EQ(route.dst().conn_id(), 20u);
        EXPECT_EQ(route.dst().module_name(), "AGC");
        EXPECT_EQ(route.dst().conn_name(), "g-1");
        EXPECT_EQ(route.dst().tag(), "P_CMD");
        return grpc::Status::OK;
      }));

  auto config = MakeConfig("IEC104", "line-1", "AGC", "g-1");
  EXPECT_TRUE(ConfigPusher::ApplyDataCenterConfig(config, stub.get()));
  EXPECT_EQ(connTagsCallCount, 2);
}

// 验证：DataCenter 会清空 jsonc 未声明连接的旧标签，并将旧路由收敛为 jsonc 目标态。
TEST(ConfigPusherDataCenterTest, ClearsStaleConnTagsAndConvergesRoutesToJsoncTarget) {
  auto stub = std::make_unique<DataCenterProto::MockDataCenterServiceStub>();

  DataCenterProto::ListConnectionsResponse connections;
  *connections.add_conns() = MakeConnection(10, "IEC104", "line-1");
  *connections.add_conns() = MakeConnection(20, "AGC", "g-1");
  *connections.add_conns() = MakeConnection(30, "ModbusRTU", "legacy");

  std::unordered_map<uint32_t, std::vector<std::string>> connTagsByConnId{
      {10, {"OLD_A", "OLD_B"}},
      {30, {"OLD_LEGACY"}}};
  std::vector<DataCenterProto::Route> routes{
      MakeResolvedRoute(30, "ModbusRTU", "legacy", "OLD_LEGACY", 20, "AGC", "g-1", "P_CMD")};

  bool updatedTargetConnTags = false;
  bool updatedDstConnTags = false;
  bool clearedLegacyConnTags = false;

  EXPECT_CALL(*stub, ListConnections(_, _, _))
      .Times(AtLeast(1))
      .WillRepeatedly(Invoke([&](grpc::ClientContext*,
                                 const DataCenterProto::Empty&,
                                 DataCenterProto::ListConnectionsResponse* resp) {
        *resp = connections;
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, GetConnTags(_, _, _))
      .Times(AnyNumber())
      .WillRepeatedly(Invoke([&](grpc::ClientContext*,
                                 const DataCenterProto::GetConnTagsRequest& req,
                                 DataCenterProto::ConnTags* resp) {
        resp->Clear();
        resp->set_conn_id(req.conn_id());
        auto it = connTagsByConnId.find(req.conn_id());
        if (it != connTagsByConnId.end()) {
          for (const auto& tag : it->second) {
            resp->add_tags(tag);
          }
        }
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, ListRoutes(_, _, _))
      .Times(AnyNumber())
      .WillRepeatedly(Invoke([&](grpc::ClientContext*,
                                 const DataCenterProto::ListRoutesRequest& req,
                                 DataCenterProto::ListRoutesResponse* resp) {
        resp->Clear();
        for (const auto& route : routes) {
          if (RouteMatchesRequest(route, req)) {
            *resp->add_routes() = route;
          }
        }
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, DeleteRoutes(_, _, _))
      .Times(AnyNumber())
      .WillRepeatedly(Invoke([&](grpc::ClientContext*,
                                 const DataCenterProto::DeleteRoutesRequest& req,
                                 DataCenterProto::Empty*) {
        std::erase_if(routes, [&](const DataCenterProto::Route& existing) {
          for (const auto& route : req.routes()) {
            if (RouteEquals(existing, route)) {
              return true;
            }
          }
          return false;
        });
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, UpsertConnTags(_, _, _))
      .Times(AtLeast(2))
      .WillRepeatedly(Invoke([&](grpc::ClientContext*,
                                 const DataCenterProto::UpsertConnTagsRequest& req,
                                 DataCenterProto::Empty*) {
        EXPECT_TRUE(req.replace());
        auto tags = ToTagVector(req.tags());
        connTagsByConnId[req.conn_id()] = tags;
        if (req.conn_id() == 10) {
          EXPECT_THAT(tags, UnorderedElementsAre("P_CMD_SRC", "P_TOTAL_DST"));
          updatedTargetConnTags = true;
        }
        if (req.conn_id() == 20) {
          EXPECT_THAT(tags, UnorderedElementsAre("P_CMD"));
          updatedDstConnTags = true;
        }
        if (req.conn_id() == 30) {
          EXPECT_TRUE(tags.empty());
          clearedLegacyConnTags = true;
        }
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, UpsertRoutes(_, _, _))
      .Times(AtLeast(1))
      .WillRepeatedly(Invoke([&](grpc::ClientContext*,
                                 const DataCenterProto::UpsertRoutesRequest& req,
                                 DataCenterProto::Empty*) {
        if (req.replace()) {
          routes.clear();
        }
        for (const auto& route : req.routes()) {
          auto it = std::find_if(routes.begin(), routes.end(), [&](const DataCenterProto::Route& existing) {
            return RouteEquals(existing, route);
          });
          if (it == routes.end()) {
            routes.push_back(route);
          }
        }
        return grpc::Status::OK;
      }));

  auto config = MakeConfig("IEC104", "line-1", "AGC", "g-1");
  EXPECT_TRUE(ConfigPusher::ApplyDataCenterConfig(config, stub.get()));

  EXPECT_TRUE(updatedTargetConnTags);
  EXPECT_TRUE(updatedDstConnTags);
  EXPECT_TRUE(clearedLegacyConnTags);
  ASSERT_TRUE(connTagsByConnId.contains(10u));
  EXPECT_THAT(connTagsByConnId.at(10u), UnorderedElementsAre("P_CMD_SRC", "P_TOTAL_DST"));
  ASSERT_TRUE(connTagsByConnId.contains(20u));
  EXPECT_THAT(connTagsByConnId.at(20u), UnorderedElementsAre("P_CMD"));
  ASSERT_TRUE(connTagsByConnId.contains(30u));
  EXPECT_TRUE(connTagsByConnId.at(30u).empty());

  const auto targetRoute = MakeResolvedRoute(10, "IEC104", "line-1", "P_CMD_SRC", 20, "AGC", "g-1", "P_CMD");
  ASSERT_EQ(routes.size(), 1u);
  EXPECT_TRUE(RouteEquals(routes.front(), targetRoute));
}
