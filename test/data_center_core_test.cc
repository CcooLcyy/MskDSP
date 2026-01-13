#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "DataCenterCore.h"

namespace {
using DataCenter::DataCenterCore;

DataCenterProto::Endpoint MakeEndpoint(uint32_t connId, std::string tag) {
  DataCenterProto::Endpoint ep;
  ep.set_conn_id(connId);
  ep.set_tag(std::move(tag));
  return ep;
}

DataCenterProto::Route MakeRoute(uint32_t srcConnId, std::string srcTag, uint32_t dstConnId, std::string dstTag) {
  DataCenterProto::Route route;
  *route.mutable_src() = MakeEndpoint(srcConnId, std::move(srcTag));
  *route.mutable_dst() = MakeEndpoint(dstConnId, std::move(dstTag));
  return route;
}
}  // namespace

TEST(DataCenterCoreTest, UpsertRoutesValidatesAgainstPointTableWhenPresent) {
  DataCenterCore core;

  DataCenterProto::UpsertPointTableRequest pt;
  pt.set_conn_id(1);
  pt.set_replace(true);
  pt.add_tags("源点");
  ASSERT_TRUE(core.UpsertPointTable(pt).ok());

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "不存在的点", 2, "目的点");

  auto status = core.UpsertRoutes(routes);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(DataCenterCoreTest, PublishRoutesWithTagRewriteOneToOne) {
  DataCenterCore core;

  DataCenterProto::UpsertPointTableRequest srcPt;
  srcPt.set_conn_id(1);
  srcPt.set_replace(true);
  srcPt.add_tags("温度A");
  ASSERT_TRUE(core.UpsertPointTable(srcPt).ok());

  DataCenterProto::UpsertPointTableRequest dstPt;
  dstPt.set_conn_id(2);
  dstPt.set_replace(true);
  dstPt.add_tags("温度B");
  ASSERT_TRUE(core.UpsertPointTable(dstPt).ok());

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "温度A", 2, "温度B");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::PublishRequest pub;
  pub.set_conn_id(1);
  pub.set_tag("温度A");
  pub.mutable_value()->set_int_value(42);

  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(core.Publish(pub, &updates).ok());
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates[0].src_conn_id(), 1u);
  EXPECT_EQ(updates[0].src_tag(), "温度A");
  EXPECT_EQ(updates[0].dst_conn_id(), 2u);
  EXPECT_EQ(updates[0].dst_tag(), "温度B");
  EXPECT_EQ(updates[0].value().int_value(), 42);
  EXPECT_GT(updates[0].ts_ms(), 0);
}

TEST(DataCenterCoreTest, PublishRoutesOneToMany) {
  DataCenterCore core;

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "功率", 2, "P");
  *routes.add_routes() = MakeRoute(1, "功率", 3, "功率");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::PublishRequest pub;
  pub.set_conn_id(1);
  pub.set_tag("功率");
  pub.mutable_value()->set_double_value(12.5);

  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(core.Publish(pub, &updates).ok());
  ASSERT_EQ(updates.size(), 2u);

  std::vector<std::pair<uint32_t, std::string>> dsts;
  dsts.emplace_back(updates[0].dst_conn_id(), updates[0].dst_tag());
  dsts.emplace_back(updates[1].dst_conn_id(), updates[1].dst_tag());
  std::sort(dsts.begin(), dsts.end());

  EXPECT_EQ(dsts[0], (std::pair<uint32_t, std::string>{2u, "P"}));
  EXPECT_EQ(dsts[1], (std::pair<uint32_t, std::string>{3u, "功率"}));
}

TEST(DataCenterCoreTest, GetLatestReturnsLastRoutedValueByDstEndpoint) {
  DataCenterCore core;

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "电压", 2, "U");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::PublishRequest pub;
  pub.set_conn_id(1);
  pub.set_tag("电压");
  pub.mutable_value()->set_int_value(220);

  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(core.Publish(pub, &updates).ok());

  DataCenterProto::GetLatestRequest latestReq;
  latestReq.set_conn_id(2);
  DataCenterProto::GetLatestResponse latestResp;
  ASSERT_TRUE(core.GetLatest(latestReq, &latestResp).ok());
  ASSERT_EQ(latestResp.updates_size(), 1);
  EXPECT_EQ(latestResp.updates(0).dst_tag(), "U");
  EXPECT_EQ(latestResp.updates(0).value().int_value(), 220);
  EXPECT_EQ(latestResp.updates(0).src_conn_id(), 1u);
  EXPECT_EQ(latestResp.updates(0).src_tag(), "电压");
}
