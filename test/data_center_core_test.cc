#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <string>
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

DataCenterProto::Endpoint MakeStableEndpoint(uint32_t connId, std::string moduleName, std::string connName, std::string tag) {
  DataCenterProto::Endpoint ep;
  ep.set_conn_id(connId);
  ep.set_module_name(std::move(moduleName));
  ep.set_conn_name(std::move(connName));
  ep.set_tag(std::move(tag));
  return ep;
}

DataCenterProto::Route MakeRoute(uint32_t srcConnId, std::string srcTag, uint32_t dstConnId, std::string dstTag) {
  DataCenterProto::Route route;
  *route.mutable_src() = MakeEndpoint(srcConnId, std::move(srcTag));
  *route.mutable_dst() = MakeEndpoint(dstConnId, std::move(dstTag));
  return route;
}

DataCenterProto::Route MakeStableRoute(uint32_t srcConnId, std::string srcModuleName, std::string srcConnName, std::string srcTag,
                                       uint32_t dstConnId, std::string dstModuleName, std::string dstConnName, std::string dstTag) {
  DataCenterProto::Route route;
  *route.mutable_src() = MakeStableEndpoint(srcConnId, std::move(srcModuleName), std::move(srcConnName), std::move(srcTag));
  *route.mutable_dst() = MakeStableEndpoint(dstConnId, std::move(dstModuleName), std::move(dstConnName), std::move(dstTag));
  return route;
}

DataCenterProto::ConnectionKey MakeConnKey(std::string moduleName, std::string connName) {
  DataCenterProto::ConnectionKey key;
  key.set_module_name(std::move(moduleName));
  key.set_conn_name(std::move(connName));
  return key;
}

DataCenterProto::ConnectionInfo MakeConnInfo(uint32_t connId, std::string moduleName, std::string connName) {
  DataCenterProto::ConnectionInfo info;
  info.set_conn_id(connId);
  info.set_module_name(std::move(moduleName));
  info.set_conn_name(std::move(connName));
  return info;
}

void InstallRouteConnections(DataCenterCore &core, std::initializer_list<uint32_t> connIds) {
  DataCenterProto::ConnectionsConfig cfg;
  uint32_t maxConnId = 0;
  for (auto connId : connIds) {
    maxConnId = std::max(maxConnId, connId);
    *cfg.add_conns() = MakeConnInfo(connId, "TestModule", "conn-" + std::to_string(connId));
  }
  cfg.set_next_conn_id(maxConnId + 1);
  ASSERT_TRUE(core.ReplaceConnectionsConfig(cfg).ok());
}
}  // 命名空间结束

// 验证：GetOrCreateConnection 对相同 (module_name, conn_name) 返回稳定的 conn_id。
TEST(DataCenterCoreTest, GetOrCreateConnectionReturnsStableConnIdByKey) {
  DataCenterCore core;

  DataCenterProto::GetOrCreateConnectionRequest req;
  *req.mutable_key() = MakeConnKey("IEC104", "104-1");

  DataCenterProto::ConnectionInfo conn1;
  ASSERT_TRUE(core.GetOrCreateConnection(req, &conn1).ok());
  EXPECT_GT(conn1.conn_id(), 0u);
  EXPECT_EQ(conn1.module_name(), "IEC104");
  EXPECT_EQ(conn1.conn_name(), "104-1");

  DataCenterProto::ConnectionInfo conn2;
  ASSERT_TRUE(core.GetOrCreateConnection(req, &conn2).ok());
  EXPECT_EQ(conn2.conn_id(), conn1.conn_id());
}

// 验证：RenameConnection 保持 conn_id 不变，且旧 key 不再可查询。
TEST(DataCenterCoreTest, RenameConnectionKeepsConnId) {
  DataCenterCore core;

  DataCenterProto::GetOrCreateConnectionRequest createReq;
  *createReq.mutable_key() = MakeConnKey("ModbusRTU", "mb-1");

  DataCenterProto::ConnectionInfo created;
  ASSERT_TRUE(core.GetOrCreateConnection(createReq, &created).ok());

  DataCenterProto::RenameConnectionRequest renameReq;
  *renameReq.mutable_old_key() = MakeConnKey("ModbusRTU", "mb-1");
  *renameReq.mutable_new_key() = MakeConnKey("ModbusRTU", "mb-rename");

  DataCenterProto::ConnectionInfo renamed;
  ASSERT_TRUE(core.RenameConnection(renameReq, &renamed).ok());
  EXPECT_EQ(renamed.conn_id(), created.conn_id());
  EXPECT_EQ(renamed.module_name(), "ModbusRTU");
  EXPECT_EQ(renamed.conn_name(), "mb-rename");

  DataCenterProto::ConnectionInfo gotOld;
  auto oldStatus = core.GetConnectionByKey(renameReq.old_key(), &gotOld);
  EXPECT_FALSE(oldStatus.ok());
  EXPECT_EQ(oldStatus.error_code(), grpc::StatusCode::NOT_FOUND);

  DataCenterProto::GetOrCreateConnectionRequest getReq;
  *getReq.mutable_key() = renameReq.new_key();
  DataCenterProto::ConnectionInfo gotNew;
  ASSERT_TRUE(core.GetOrCreateConnection(getReq, &gotNew).ok());
  EXPECT_EQ(gotNew.conn_id(), created.conn_id());
}

// 验证：DeleteConnection 会清理该连接关联的连接标签注册表/路由/源端与目的端最新值缓存。
TEST(DataCenterCoreTest, DeleteConnectionCleansConnTagsRoutesAndLatest) {
  DataCenterCore core;

  DataCenterProto::GetOrCreateConnectionRequest createSrc;
  *createSrc.mutable_key() = MakeConnKey("ModbusRTU", "mb-src");
  DataCenterProto::ConnectionInfo src;
  ASSERT_TRUE(core.GetOrCreateConnection(createSrc, &src).ok());

  DataCenterProto::GetOrCreateConnectionRequest createDst;
  *createDst.mutable_key() = MakeConnKey("IEC104", "104-dst");
  DataCenterProto::ConnectionInfo dst;
  ASSERT_TRUE(core.GetOrCreateConnection(createDst, &dst).ok());

  DataCenterProto::UpsertConnTagsRequest srcPt;
  srcPt.set_conn_id(src.conn_id());
  srcPt.set_replace(true);
  srcPt.add_tags("A");
  ASSERT_TRUE(core.UpsertConnTags(srcPt).ok());

  DataCenterProto::UpsertConnTagsRequest dstPt;
  dstPt.set_conn_id(dst.conn_id());
  dstPt.set_replace(true);
  dstPt.add_tags("B");
  ASSERT_TRUE(core.UpsertConnTags(dstPt).ok());

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(src.conn_id(), "A", dst.conn_id(), "B");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::PublishRequest pub;
  pub.set_conn_id(src.conn_id());
  pub.set_tag("A");
  pub.mutable_value()->set_int_value(123);

  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(core.Publish(pub, &updates).ok());
  ASSERT_EQ(updates.size(), 1u);

  DataCenterProto::GetLatestRequest latestReq;
  latestReq.set_conn_id(dst.conn_id());
  DataCenterProto::GetLatestResponse latestResp;
  ASSERT_TRUE(core.GetLatest(latestReq, &latestResp).ok());
  ASSERT_EQ(latestResp.updates_size(), 1);

  DataCenterProto::GetSourceLatestRequest sourceLatestReq;
  sourceLatestReq.set_conn_id(src.conn_id());
  DataCenterProto::GetSourceLatestResponse sourceLatestResp;
  ASSERT_TRUE(core.GetSourceLatest(sourceLatestReq, &sourceLatestResp).ok());
  ASSERT_EQ(sourceLatestResp.updates_size(), 1);

  DataCenterProto::DeleteConnectionRequest delReq;
  *delReq.mutable_key() = createSrc.key();
  ASSERT_TRUE(core.DeleteConnection(delReq).ok());

  DataCenterProto::ConnTags table;
  auto ptStatus = core.GetConnTags(src.conn_id(), &table);
  EXPECT_FALSE(ptStatus.ok());
  EXPECT_EQ(ptStatus.error_code(), grpc::StatusCode::NOT_FOUND);

  DataCenterProto::ListRoutesRequest listReq;
  auto listResp = core.ListRoutes(listReq);
  EXPECT_EQ(listResp.routes_size(), 0);

  DataCenterProto::GetLatestResponse afterDel;
  ASSERT_TRUE(core.GetLatest(latestReq, &afterDel).ok());
  EXPECT_EQ(afterDel.updates_size(), 0);

  DataCenterProto::GetSourceLatestResponse afterSourceDel;
  auto sourceStatus = core.GetSourceLatest(sourceLatestReq, &afterSourceDel);
  EXPECT_FALSE(sourceStatus.ok());
  EXPECT_EQ(sourceStatus.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：当连接标签注册表存在时，UpsertRoutes 会校验 tag 必须在注册表内。
TEST(DataCenterCoreTest, UpsertRoutesValidatesAgainstConnTagsWhenPresent) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

  DataCenterProto::UpsertConnTagsRequest pt;
  pt.set_conn_id(1);
  pt.set_replace(true);
  pt.add_tags("源点");
  ASSERT_TRUE(core.UpsertConnTags(pt).ok());

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "不存在的点", 2, "目的点");

  auto status = core.UpsertRoutes(routes);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：replace=true 的 UpsertRoutes 校验失败时不会清空已有路由。
TEST(DataCenterCoreTest, UpsertRoutesReplaceFailureKeepsExistingRoutes) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

  DataCenterProto::UpsertConnTagsRequest srcTags;
  srcTags.set_conn_id(1);
  srcTags.set_replace(true);
  srcTags.add_tags("A");
  ASSERT_TRUE(core.UpsertConnTags(srcTags).ok());

  DataCenterProto::UpsertRoutesRequest initial;
  initial.set_replace(true);
  *initial.add_routes() = MakeRoute(1, "A", 2, "B");
  ASSERT_TRUE(core.UpsertRoutes(initial).ok());

  DataCenterProto::UpsertRoutesRequest replacement;
  replacement.set_replace(true);
  *replacement.add_routes() = MakeRoute(1, "不存在的点", 2, "C");

  auto status = core.UpsertRoutes(replacement);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  DataCenterProto::ListRoutesRequest listReq;
  auto listResp = core.ListRoutes(listReq);
  ASSERT_EQ(listResp.routes_size(), 1);
  EXPECT_EQ(listResp.routes(0).src().conn_id(), 1u);
  EXPECT_EQ(listResp.routes(0).src().tag(), "A");
  EXPECT_EQ(listResp.routes(0).dst().conn_id(), 2u);
  EXPECT_EQ(listResp.routes(0).dst().tag(), "B");

  DataCenterProto::PublishRequest pub;
  pub.set_conn_id(1);
  pub.set_tag("A");
  pub.mutable_value()->set_int_value(1);
  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(core.Publish(pub, &updates).ok());
  EXPECT_EQ(updates.size(), 1u);
}

// 验证：增量 UpsertRoutes 校验失败时不会留下已经处理过的部分路由。
TEST(DataCenterCoreTest, UpsertRoutesIncrementalFailureDoesNotKeepPartialRoutes) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

  DataCenterProto::UpsertConnTagsRequest srcTags;
  srcTags.set_conn_id(1);
  srcTags.set_replace(true);
  srcTags.add_tags("A");
  srcTags.add_tags("C");
  ASSERT_TRUE(core.UpsertConnTags(srcTags).ok());

  DataCenterProto::UpsertRoutesRequest initial;
  initial.set_replace(true);
  *initial.add_routes() = MakeRoute(1, "A", 2, "B");
  ASSERT_TRUE(core.UpsertRoutes(initial).ok());

  DataCenterProto::UpsertRoutesRequest incremental;
  incremental.set_replace(false);
  *incremental.add_routes() = MakeRoute(1, "C", 2, "D");
  *incremental.add_routes() = MakeRoute(1, "不存在的点", 2, "E");

  auto status = core.UpsertRoutes(incremental);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  DataCenterProto::ListRoutesRequest listReq;
  auto listResp = core.ListRoutes(listReq);
  ASSERT_EQ(listResp.routes_size(), 1);
  EXPECT_EQ(listResp.routes(0).src().tag(), "A");
  EXPECT_EQ(listResp.routes(0).dst().tag(), "B");

  DataCenterProto::PublishRequest pub;
  pub.set_conn_id(1);
  pub.set_tag("C");
  pub.mutable_value()->set_int_value(2);
  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(core.Publish(pub, &updates).ok());
  EXPECT_TRUE(updates.empty());
}

// 验证：Publish 按路由进行 tag 重写（srcTag -> dstTag）的一对一转发。
TEST(DataCenterCoreTest, PublishRoutesWithTagRewriteOneToOne) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

  DataCenterProto::UpsertConnTagsRequest srcPt;
  srcPt.set_conn_id(1);
  srcPt.set_replace(true);
  srcPt.add_tags("温度A");
  ASSERT_TRUE(core.UpsertConnTags(srcPt).ok());

  DataCenterProto::UpsertConnTagsRequest dstPt;
  dstPt.set_conn_id(2);
  dstPt.set_replace(true);
  dstPt.add_tags("温度B");
  ASSERT_TRUE(core.UpsertConnTags(dstPt).ok());

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

// 验证：Publish 支持一对多路由，生成多个目的端点的更新。
TEST(DataCenterCoreTest, PublishRoutesOneToMany) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2, 3});

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

// 验证：同步命令匹配到多个正常业务目的端时必须拒绝歧义，不再特殊处理任何目的模块。
TEST(DataCenterCoreTest, ResolveCommandRouteRejectsMultipleBusinessDestinations) {
  DataCenterCore core;

  DataCenterProto::ConnectionsConfig cfg;
  cfg.set_next_conn_id(4);
  *cfg.add_conns() = MakeConnInfo(1, "IEC104", "104主站");
  *cfg.add_conns() = MakeConnInfo(2, "AGC", "AGC");
  *cfg.add_conns() = MakeConnInfo(3, "AVC", "AVC");
  ASSERT_TRUE(core.ReplaceConnectionsConfig(cfg).ok());

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeStableRoute(1, "IEC104", "104主站", "AGC_AGC_AGC总控点",
                                         2, "AGC", "AGC", "AGC总控点");
  *routes.add_routes() = MakeStableRoute(1, "IEC104", "104主站", "AGC_AGC_AGC总控点",
                                         3, "AVC", "AVC", "AVC总控点");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::ExecuteCommandRequest req;
  req.mutable_src()->set_conn_id(1);
  req.mutable_src()->set_tag("AGC_AGC_AGC总控点");
  req.mutable_value()->set_double_value(500.0);
  req.set_quality(DataCenterProto::QUALITY_GOOD);

  DataCenterProto::ExecuteCommandResponse resp;
  ASSERT_TRUE(core.ResolveCommandRoute(req, &resp).ok());
  EXPECT_EQ(resp.status(), DataCenterProto::COMMAND_AMBIGUOUS_ROUTE);
  EXPECT_TRUE(resp.dst().module_name().empty());
}

// 验证：GetLatest 返回目标连接内“按目的端点”最新一次路由后的值（按 dst_tag 排序）。
TEST(DataCenterCoreTest, GetLatestReturnsLastRoutedValueByDstEndpoint) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

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

// 验证：GetSourceLatest 可读取无路由源点、按 tag 排序，并返回进程内全局递增 sequence。
TEST(DataCenterCoreTest, GetSourceLatestReturnsUnroutedPointsWithGlobalSequence) {
  DataCenterCore core;
  InstallRouteConnections(core, {1});

  DataCenterProto::PublishRequest pubZ;
  pubZ.set_conn_id(1);
  pubZ.set_tag("Z");
  pubZ.mutable_value()->set_int_value(10);
  pubZ.set_ts_ms(1001);
  pubZ.set_quality(DataCenterProto::QUALITY_GOOD);

  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(core.Publish(pubZ, &updates).ok());
  EXPECT_TRUE(updates.empty());

  DataCenterProto::PublishRequest pubA;
  pubA.set_conn_id(1);
  pubA.set_tag("A");
  pubA.mutable_value()->set_double_value(20.5);
  pubA.set_ts_ms(1002);
  pubA.set_quality(DataCenterProto::QUALITY_UNCERTAIN);
  ASSERT_TRUE(core.Publish(pubA, &updates).ok());
  EXPECT_TRUE(updates.empty());

  DataCenterProto::GetSourceLatestRequest req;
  req.set_conn_id(1);
  DataCenterProto::GetSourceLatestResponse resp;
  ASSERT_TRUE(core.GetSourceLatest(req, &resp).ok());
  ASSERT_EQ(resp.updates_size(), 2);
  EXPECT_EQ(resp.updates(0).tag(), "A");
  EXPECT_DOUBLE_EQ(resp.updates(0).value().double_value(), 20.5);
  EXPECT_EQ(resp.updates(0).ts_ms(), 1002);
  EXPECT_EQ(resp.updates(0).quality(), DataCenterProto::QUALITY_UNCERTAIN);
  EXPECT_EQ(resp.updates(1).tag(), "Z");
  EXPECT_EQ(resp.updates(1).value().int_value(), 10);
  EXPECT_EQ(resp.updates(1).ts_ms(), 1001);
  EXPECT_EQ(resp.updates(1).quality(), DataCenterProto::QUALITY_GOOD);
  EXPECT_LT(resp.updates(1).sequence(), resp.updates(0).sequence());

  DataCenterProto::GetSourceLatestRequest filteredReq;
  filteredReq.set_conn_id(1);
  filteredReq.add_tags("Z");
  DataCenterProto::GetSourceLatestResponse filteredResp;
  ASSERT_TRUE(core.GetSourceLatest(filteredReq, &filteredResp).ok());
  ASSERT_EQ(filteredResp.updates_size(), 1);
  EXPECT_EQ(filteredResp.updates(0).tag(), "Z");
  EXPECT_EQ(filteredResp.updates(0).sequence(), resp.updates(1).sequence());
}

// 验证：源端最新值不写入 DataCenterState，按持久化配置恢复后不会带回进程内实时缓存。
TEST(DataCenterCoreTest, GetSourceLatestCacheIsNotRestoredFromPersistentState) {
  DataCenterCore core;
  InstallRouteConnections(core, {1});

  DataCenterProto::PublishRequest pub;
  pub.set_conn_id(1);
  pub.set_tag("瞬时功率");
  pub.mutable_value()->set_double_value(88.8);
  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(core.Publish(pub, &updates).ok());

  DataCenterProto::DataCenterState persistedState;
  *persistedState.mutable_connections() = core.DumpConnectionsConfig();
  *persistedState.mutable_conn_tags() = core.DumpConnTagsConfig();
  *persistedState.mutable_routes() = core.DumpRoutesConfig();

  DataCenterCore restored;
  ASSERT_TRUE(restored.ReplaceConnectionsConfig(persistedState.connections()).ok());
  ASSERT_TRUE(restored.ReplaceConnTagsConfig(persistedState.conn_tags()).ok());
  ASSERT_TRUE(restored.ReplaceRoutesConfig(persistedState.routes()).ok());

  DataCenterProto::GetSourceLatestRequest req;
  req.set_conn_id(1);
  DataCenterProto::GetSourceLatestResponse resp;
  ASSERT_TRUE(restored.GetSourceLatest(req, &resp).ok());
  EXPECT_EQ(resp.updates_size(), 0);
}

// 验证：BatchPublish 在输入合法时会发布全部点并生成对应路由更新。
TEST(DataCenterCoreTest, BatchPublishPublishesAllPointsWhenValid) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "A", 2, "B");
  *routes.add_routes() = MakeRoute(1, "C", 2, "D");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::BatchPublishRequest batch;
  auto* p1 = batch.add_points();
  p1->set_conn_id(1);
  p1->set_tag("A");
  p1->mutable_value()->set_int_value(10);

  auto* p2 = batch.add_points();
  p2->set_conn_id(1);
  p2->set_tag("C");
  p2->mutable_value()->set_int_value(20);

  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(core.BatchPublish(batch, &updates).ok());
  ASSERT_EQ(updates.size(), 2u);

  DataCenterProto::GetLatestRequest latestReq;
  latestReq.set_conn_id(2);
  DataCenterProto::GetLatestResponse latestResp;
  ASSERT_TRUE(core.GetLatest(latestReq, &latestResp).ok());
  ASSERT_EQ(latestResp.updates_size(), 2);
  EXPECT_EQ(latestResp.updates(0).dst_tag(), "B");
  EXPECT_EQ(latestResp.updates(0).value().int_value(), 10);
  EXPECT_EQ(latestResp.updates(1).dst_tag(), "D");
  EXPECT_EQ(latestResp.updates(1).value().int_value(), 20);

  DataCenterProto::GetSourceLatestRequest sourceLatestReq;
  sourceLatestReq.set_conn_id(1);
  DataCenterProto::GetSourceLatestResponse sourceLatestResp;
  ASSERT_TRUE(core.GetSourceLatest(sourceLatestReq, &sourceLatestResp).ok());
  ASSERT_EQ(sourceLatestResp.updates_size(), 2);
  EXPECT_EQ(sourceLatestResp.updates(0).tag(), "A");
  EXPECT_EQ(sourceLatestResp.updates(0).value().int_value(), 10);
  EXPECT_EQ(sourceLatestResp.updates(1).tag(), "C");
  EXPECT_EQ(sourceLatestResp.updates(1).value().int_value(), 20);
  EXPECT_LT(sourceLatestResp.updates(0).sequence(), sourceLatestResp.updates(1).sequence());
}

// 验证：BatchPublish 在校验失败时具有原子性（不输出 updates，且不更新源端或目的端 latest）。
TEST(DataCenterCoreTest, BatchPublishIsAtomicAndDoesNotUpdateLatestOnValidationFailure) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "A", 2, "B");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::BatchPublishRequest batch;
  auto* p1 = batch.add_points();
  p1->set_conn_id(1);
  p1->set_tag("A");
  p1->mutable_value()->set_int_value(10);

  auto* p2 = batch.add_points();
  p2->set_conn_id(1);
  p2->set_tag("");
  p2->mutable_value()->set_int_value(20);

  std::vector<DataCenterProto::PointUpdate> updates;
  auto status = core.BatchPublish(batch, &updates);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(updates.empty());

  DataCenterProto::GetLatestRequest latestReq;
  latestReq.set_conn_id(2);
  DataCenterProto::GetLatestResponse latestResp;
  ASSERT_TRUE(core.GetLatest(latestReq, &latestResp).ok());
  EXPECT_EQ(latestResp.updates_size(), 0);

  DataCenterProto::GetSourceLatestRequest sourceLatestReq;
  sourceLatestReq.set_conn_id(1);
  DataCenterProto::GetSourceLatestResponse sourceLatestResp;
  ASSERT_TRUE(core.GetSourceLatest(sourceLatestReq, &sourceLatestResp).ok());
  EXPECT_EQ(sourceLatestResp.updates_size(), 0);
}

// 验证：DumpConnTagsConfig 与 ReplaceConnTagsConfig 可 roundtrip 恢复连接标签注册表配置。
TEST(DataCenterCoreTest, DumpAndReplaceConnTagsConfigRoundtrip) {
  DataCenterCore core;
  InstallRouteConnections(core, {1});

  DataCenterProto::UpsertConnTagsRequest pt;
  pt.set_conn_id(1);
  pt.set_replace(true);
  pt.add_tags("点1");
  pt.add_tags("点2");
  ASSERT_TRUE(core.UpsertConnTags(pt).ok());

  auto config = core.DumpConnTagsConfig();

  DataCenterCore restored;
  InstallRouteConnections(restored, {1});
  ASSERT_TRUE(restored.ReplaceConnTagsConfig(config).ok());

  DataCenterProto::ConnTags table;
  ASSERT_TRUE(restored.GetConnTags(1, &table).ok());
  EXPECT_EQ(table.module_name(), "TestModule");
  EXPECT_EQ(table.conn_name(), "conn-1");
  ASSERT_EQ(table.tags_size(), 2);
  EXPECT_EQ(table.tags(0), "点1");
  EXPECT_EQ(table.tags(1), "点2");
}

// 验证：覆盖连接标签注册表时，会删除引用已移除 tag 的路由，保证连接、tag、路由保持对齐。
TEST(DataCenterCoreTest, UpsertConnTagsReplacePrunesRoutesReferencingRemovedTags) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

  DataCenterProto::UpsertConnTagsRequest srcTags;
  srcTags.set_conn_id(1);
  srcTags.set_replace(true);
  srcTags.add_tags("A");
  srcTags.add_tags("C");
  ASSERT_TRUE(core.UpsertConnTags(srcTags).ok());

  DataCenterProto::UpsertConnTagsRequest dstTags;
  dstTags.set_conn_id(2);
  dstTags.set_replace(true);
  dstTags.add_tags("B");
  dstTags.add_tags("D");
  ASSERT_TRUE(core.UpsertConnTags(dstTags).ok());

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "A", 2, "B");
  *routes.add_routes() = MakeRoute(1, "C", 2, "D");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::UpsertConnTagsRequest replacement;
  replacement.set_conn_id(1);
  replacement.set_replace(true);
  replacement.add_tags("A");
  ASSERT_TRUE(core.UpsertConnTags(replacement).ok());

  DataCenterProto::ListRoutesRequest listReq;
  const auto resp = core.ListRoutes(listReq);
  ASSERT_EQ(resp.routes_size(), 1);
  EXPECT_EQ(resp.routes(0).src().tag(), "A");
  EXPECT_EQ(resp.routes(0).dst().tag(), "B");
}

// 验证：ReplaceConnTagsConfig 会按稳定连接主键合并并去重 tags。
TEST(DataCenterCoreTest, ReplaceConnTagsConfigMergesAndDeduplicatesByStableConnectionKey) {
  DataCenterCore core;
  InstallRouteConnections(core, {1});

  DataCenterProto::ConnTagsConfig config;
  auto* t1 = config.add_conn_tags();
  t1->set_conn_id(1);
  t1->set_module_name("TestModule");
  t1->set_conn_name("conn-1");
  t1->add_tags("A");
  t1->add_tags("B");

  auto* t2 = config.add_conn_tags();
  t2->set_conn_id(99);
  t2->set_module_name("TestModule");
  t2->set_conn_name("conn-1");
  t2->add_tags("B");
  t2->add_tags("C");

  ASSERT_TRUE(core.ReplaceConnTagsConfig(config).ok());

  DataCenterProto::ConnTags table;
  ASSERT_TRUE(core.GetConnTags(1, &table).ok());
  ASSERT_EQ(table.tags_size(), 3);
  EXPECT_EQ(table.tags(0), "A");
  EXPECT_EQ(table.tags(1), "B");
  EXPECT_EQ(table.tags(2), "C");
}

// 验证：DumpRoutesConfig 与 ReplaceRoutesConfig 可 roundtrip 恢复路由配置。
TEST(DataCenterCoreTest, DumpAndReplaceRoutesConfigRoundtrip) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2, 3});

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "源点", 2, "目的点");
  *routes.add_routes() = MakeRoute(1, "源点", 3, "目的点2");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  auto config = core.DumpRoutesConfig();

  DataCenterCore restored;
  InstallRouteConnections(restored, {1, 2, 3});
  ASSERT_TRUE(restored.ReplaceRoutesConfig(config).ok());

  DataCenterProto::ListRoutesRequest req;
  auto resp = restored.ListRoutes(req);
  ASSERT_EQ(resp.routes_size(), 2);
  EXPECT_EQ(resp.routes(0).src().conn_id(), 1u);
  EXPECT_EQ(resp.routes(0).src().tag(), "源点");
  EXPECT_EQ(resp.routes(0).dst().conn_id(), 2u);
  EXPECT_EQ(resp.routes(0).dst().tag(), "目的点");
  EXPECT_EQ(resp.routes(1).dst().conn_id(), 3u);
  EXPECT_EQ(resp.routes(1).dst().tag(), "目的点2");
}

// 验证：稳定路由以 module_name/conn_name/tag 为长期身份，连接 ID 重排后仍能正确转发。
TEST(DataCenterCoreTest, StableRoutesSurviveConnIdReallocation) {
  DataCenterCore core;

  DataCenterProto::ConnectionsConfig cfg;
  cfg.set_next_conn_id(21);
  *cfg.add_conns() = MakeConnInfo(10, "DLT645", "lora-1");
  *cfg.add_conns() = MakeConnInfo(20, "IEC104", "upper");
  ASSERT_TRUE(core.ReplaceConnectionsConfig(cfg).ok());

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeStableRoute(10, "DLT645", "lora-1", "A相电压", 20, "IEC104", "upper", "Uab");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());
  const auto stableConfig = core.DumpRoutesConfig();

  DataCenterCore restored;
  DataCenterProto::ConnectionsConfig remapped;
  remapped.set_next_conn_id(102);
  *remapped.add_conns() = MakeConnInfo(101, "DLT645", "lora-1");
  *remapped.add_conns() = MakeConnInfo(102, "IEC104", "upper");
  ASSERT_TRUE(restored.ReplaceConnectionsConfig(remapped).ok());
  ASSERT_TRUE(restored.ReplaceRoutesConfig(stableConfig).ok());

  DataCenterProto::PublishRequest pub;
  pub.set_conn_id(101);
  pub.set_tag("A相电压");
  pub.mutable_value()->set_double_value(220.5);

  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(restored.Publish(pub, &updates).ok());
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates[0].src_conn_id(), 101u);
  EXPECT_EQ(updates[0].src_tag(), "A相电压");
  EXPECT_EQ(updates[0].dst_conn_id(), 102u);
  EXPECT_EQ(updates[0].dst_tag(), "Uab");
  EXPECT_EQ(updates[0].value().double_value(), 220.5);
}

// 验证：稳定路由中的旧 conn_id 与当前连接表不一致时，按稳定连接主键恢复并使用当前 conn_id 转发。
TEST(DataCenterCoreTest, StableRoutesIgnoreStaleConnIdWhenRestoring) {
  DataCenterProto::RoutesConfig config;
  *config.add_routes() = MakeStableRoute(1, "AGC", "控制组2", "AGC总有功测量点",
                                         2, "IEC104", "IEC104", "AGC_控制组1_AGC总有功测量点");

  DataCenterCore restored;
  DataCenterProto::ConnectionsConfig remapped;
  remapped.set_next_conn_id(4);
  *remapped.add_conns() = MakeConnInfo(1, "IEC104", "IEC104");
  *remapped.add_conns() = MakeConnInfo(3, "AGC", "控制组2");
  ASSERT_TRUE(restored.ReplaceConnectionsConfig(remapped).ok());
  ASSERT_TRUE(restored.ReplaceRoutesConfig(config).ok());

  DataCenterProto::ListRoutesRequest listReq;
  const auto listResp = restored.ListRoutes(listReq);
  ASSERT_EQ(listResp.routes_size(), 1);
  EXPECT_EQ(listResp.routes(0).src().conn_id(), 3u);
  EXPECT_EQ(listResp.routes(0).src().module_name(), "AGC");
  EXPECT_EQ(listResp.routes(0).src().conn_name(), "控制组2");
  EXPECT_EQ(listResp.routes(0).dst().conn_id(), 1u);
  EXPECT_EQ(listResp.routes(0).dst().module_name(), "IEC104");
  EXPECT_EQ(listResp.routes(0).dst().conn_name(), "IEC104");

  DataCenterProto::PublishRequest pub;
  pub.set_conn_id(3);
  pub.set_tag("AGC总有功测量点");
  pub.mutable_value()->set_double_value(123.0);

  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(restored.Publish(pub, &updates).ok());
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates[0].src_conn_id(), 3u);
  EXPECT_EQ(updates[0].dst_conn_id(), 1u);
  EXPECT_EQ(updates[0].dst_tag(), "AGC_控制组1_AGC总有功测量点");
}

// 验证：稳定连接主键不存在时拒绝写入，不能退回使用指向其他连接的 conn_id。
TEST(DataCenterCoreTest, StableRoutesDoNotFallbackToMismatchedStaleConnId) {
  DataCenterCore core;

  DataCenterProto::ConnectionsConfig cfg;
  cfg.set_next_conn_id(3);
  *cfg.add_conns() = MakeConnInfo(1, "IEC104", "IEC104");
  *cfg.add_conns() = MakeConnInfo(2, "ModbusRTU", "485-3");
  ASSERT_TRUE(core.ReplaceConnectionsConfig(cfg).ok());

  DataCenterProto::UpsertConnTagsRequest pt;
  pt.set_conn_id(1);
  pt.set_replace(true);
  pt.add_tags("AGC总有功测量点");
  ASSERT_TRUE(core.UpsertConnTags(pt).ok());

  DataCenterProto::RoutesConfig config;
  *config.add_routes() = MakeStableRoute(1, "AGC", "控制组2", "AGC总有功测量点",
                                         2, "IEC104", "IEC104", "AGC_控制组1_AGC总有功测量点");

  const auto status = core.ReplaceRoutesConfig(config);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：稳定路由携带旧 conn_id 时，连接标签注册表按稳定主键解析后的当前连接校验，不能被旧 conn_id 的 tags 误放行。
TEST(DataCenterCoreTest, StableRoutesValidateConnTagsByStableConnectionKeyNotStaleConnId) {
  DataCenterCore core;

  DataCenterProto::ConnectionsConfig cfg;
  cfg.set_next_conn_id(4);
  *cfg.add_conns() = MakeConnInfo(1, "IEC104", "IEC104");
  *cfg.add_conns() = MakeConnInfo(3, "AGC", "控制组2");
  ASSERT_TRUE(core.ReplaceConnectionsConfig(cfg).ok());

  DataCenterProto::UpsertConnTagsRequest staleTags;
  staleTags.set_conn_id(1);
  staleTags.set_replace(true);
  staleTags.add_tags("AGC总有功测量点");
  ASSERT_TRUE(core.UpsertConnTags(staleTags).ok());

  DataCenterProto::UpsertConnTagsRequest currentTags;
  currentTags.set_conn_id(3);
  currentTags.set_replace(true);
  currentTags.add_tags("其他点");
  ASSERT_TRUE(core.UpsertConnTags(currentTags).ok());

  DataCenterProto::RoutesConfig config;
  *config.add_routes() = MakeStableRoute(1, "AGC", "控制组2", "AGC总有功测量点",
                                         1, "IEC104", "IEC104", "AGC_控制组1_AGC总有功测量点");

  const auto status = core.ReplaceRoutesConfig(config);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：当前 conn_id 路由请求在连接注册表可用时会归一化为稳定连接主键。
TEST(DataCenterCoreTest, ConnIdRouteRequestsDumpWithStableConnectionKeysWhenConnectionsExist) {
  DataCenterCore core;

  DataCenterProto::ConnectionsConfig cfg;
  cfg.set_next_conn_id(3);
  *cfg.add_conns() = MakeConnInfo(1, "ModbusRTU", "485-3");
  *cfg.add_conns() = MakeConnInfo(2, "IEC104", "upper");
  ASSERT_TRUE(core.ReplaceConnectionsConfig(cfg).ok());

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "功率", 2, "P");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  auto dumped = core.DumpRoutesConfig();
  ASSERT_EQ(dumped.routes_size(), 1);
  EXPECT_EQ(dumped.routes(0).src().conn_id(), 1u);
  EXPECT_EQ(dumped.routes(0).src().module_name(), "ModbusRTU");
  EXPECT_EQ(dumped.routes(0).src().conn_name(), "485-3");
  EXPECT_EQ(dumped.routes(0).src().tag(), "功率");
  EXPECT_EQ(dumped.routes(0).dst().conn_id(), 2u);
  EXPECT_EQ(dumped.routes(0).dst().module_name(), "IEC104");
  EXPECT_EQ(dumped.routes(0).dst().conn_name(), "upper");
  EXPECT_EQ(dumped.routes(0).dst().tag(), "P");
}

// 验证：conn_id 路由请求在缺少连接注册表时拒绝写入，避免把不明语义的路由写入内存。
TEST(DataCenterCoreTest, ConnIdRouteRequestsFailWhenConnectionRegistryMissing) {
  DataCenterCore core;

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "功率", 2, "P");

  auto status = core.UpsertRoutes(routes);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：连接重命名后，稳定路由引用会迁移到新连接主键并继续转发。
TEST(DataCenterCoreTest, RenameConnectionRewritesStableRoutes) {
  DataCenterCore core;

  DataCenterProto::ConnectionsConfig cfg;
  cfg.set_next_conn_id(3);
  *cfg.add_conns() = MakeConnInfo(1, "ModbusRTU", "old");
  *cfg.add_conns() = MakeConnInfo(2, "IEC104", "upper");
  ASSERT_TRUE(core.ReplaceConnectionsConfig(cfg).ok());

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeStableRoute(1, "ModbusRTU", "old", "功率", 2, "IEC104", "upper", "P");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::RenameConnectionRequest rename;
  *rename.mutable_old_key() = MakeConnKey("ModbusRTU", "old");
  *rename.mutable_new_key() = MakeConnKey("ModbusRTU", "new");
  DataCenterProto::ConnectionInfo renamed;
  ASSERT_TRUE(core.RenameConnection(rename, &renamed).ok());
  EXPECT_EQ(renamed.conn_id(), 1u);

  auto dumped = core.DumpRoutesConfig();
  ASSERT_EQ(dumped.routes_size(), 1);
  EXPECT_EQ(dumped.routes(0).src().module_name(), "ModbusRTU");
  EXPECT_EQ(dumped.routes(0).src().conn_name(), "new");

  DataCenterProto::PublishRequest pub;
  pub.set_conn_id(1);
  pub.set_tag("功率");
  pub.mutable_value()->set_int_value(7);
  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(core.Publish(pub, &updates).ok());
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates[0].dst_conn_id(), 2u);
  EXPECT_EQ(updates[0].dst_tag(), "P");
}

// 验证：ReplaceRoutesConfig 会按 (src,dst) 对路由去重。
TEST(DataCenterCoreTest, ReplaceRoutesConfigDeduplicatesBySrcDst) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

  DataCenterProto::RoutesConfig config;
  *config.add_routes() = MakeRoute(1, "A", 2, "B");
  *config.add_routes() = MakeRoute(1, "A", 2, "B");

  ASSERT_TRUE(core.ReplaceRoutesConfig(config).ok());

  DataCenterProto::ListRoutesRequest req;
  auto resp = core.ListRoutes(req);
  ASSERT_EQ(resp.routes_size(), 1);
  EXPECT_EQ(resp.routes(0).src().conn_id(), 1u);
  EXPECT_EQ(resp.routes(0).src().tag(), "A");
  EXPECT_EQ(resp.routes(0).dst().conn_id(), 2u);
  EXPECT_EQ(resp.routes(0).dst().tag(), "B");
}

// 验证：当连接标签注册表存在时，ReplaceRoutesConfig 会校验 tag 必须在注册表内。
TEST(DataCenterCoreTest, ReplaceRoutesConfigValidatesAgainstConnTagsWhenPresent) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

  DataCenterProto::UpsertConnTagsRequest pt;
  pt.set_conn_id(1);
  pt.set_replace(true);
  pt.add_tags("存在的点");
  ASSERT_TRUE(core.UpsertConnTags(pt).ok());

  DataCenterProto::RoutesConfig config;
  *config.add_routes() = MakeRoute(1, "不存在的点", 2, "目的点");

  auto status = core.ReplaceRoutesConfig(config);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：ReplaceConnectionsConfig 在 next_conn_id=0 时会按 max(conn_id)+1 计算；且当 next_conn_id 太小时会向上修正。
TEST(DataCenterCoreTest, ReplaceConnectionsConfigComputesAndClampsNextConnId) {
  DataCenterCore core;

  DataCenterProto::ConnectionsConfig empty;
  empty.set_next_conn_id(0);
  ASSERT_TRUE(core.ReplaceConnectionsConfig(empty).ok());
  EXPECT_EQ(core.DumpConnectionsConfig().next_conn_id(), 1u);

  DataCenterProto::ConnectionsConfig cfg;
  cfg.set_next_conn_id(1);  // 小于 computedNext，应被修正。
  *cfg.add_conns() = MakeConnInfo(5, "M1", "C1");
  *cfg.add_conns() = MakeConnInfo(2, "M2", "C2");
  ASSERT_TRUE(core.ReplaceConnectionsConfig(cfg).ok());
  EXPECT_EQ(core.DumpConnectionsConfig().next_conn_id(), 6u);

  DataCenterProto::ConnectionsConfig keep;
  keep.set_next_conn_id(100);  // 大于 computedNext，应保持不变。
  *keep.add_conns() = MakeConnInfo(5, "M1", "C1");
  ASSERT_TRUE(core.ReplaceConnectionsConfig(keep).ok());
  EXPECT_EQ(core.DumpConnectionsConfig().next_conn_id(), 100u);
}

// 验证：ReplaceConnectionsConfig 会拒绝重复 conn_id。
TEST(DataCenterCoreTest, ReplaceConnectionsConfigRejectsDuplicateConnId) {
  DataCenterCore core;

  DataCenterProto::ConnectionsConfig cfg;
  *cfg.add_conns() = MakeConnInfo(1, "M1", "A");
  *cfg.add_conns() = MakeConnInfo(1, "M2", "B");

  auto status = core.ReplaceConnectionsConfig(cfg);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：ReplaceConnectionsConfig 会拒绝重复 (module_name, conn_name)。
TEST(DataCenterCoreTest, ReplaceConnectionsConfigRejectsDuplicateConnKey) {
  DataCenterCore core;

  DataCenterProto::ConnectionsConfig cfg;
  *cfg.add_conns() = MakeConnInfo(1, "M", "A");
  *cfg.add_conns() = MakeConnInfo(2, "M", "A");

  auto status = core.ReplaceConnectionsConfig(cfg);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：UpsertConnection 在 conn_id 未分配时返回 NOT_FOUND。
TEST(DataCenterCoreTest, UpsertConnectionReturnsNotFoundWhenConnIdNotAllocated) {
  DataCenterCore core;

  DataCenterProto::UpsertConnectionRequest req;
  *req.mutable_conn() = MakeConnInfo(123, "M", "C");

  auto status = core.UpsertConnection(req);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：UpsertConnection 会更新 key 映射，旧 key 不再可查询。
TEST(DataCenterCoreTest, UpsertConnectionUpdatesKeyMappingAndAllowsLookupByNewKey) {
  DataCenterCore core;

  DataCenterProto::GetOrCreateConnectionRequest createReq;
  *createReq.mutable_key() = MakeConnKey("M", "old");

  DataCenterProto::ConnectionInfo created;
  ASSERT_TRUE(core.GetOrCreateConnection(createReq, &created).ok());

  DataCenterProto::UpsertConnectionRequest upsertReq;
  *upsertReq.mutable_conn() = MakeConnInfo(created.conn_id(), "M", "new");
  ASSERT_TRUE(core.UpsertConnection(upsertReq).ok());

  DataCenterProto::ConnectionInfo gotOld;
  auto oldStatus = core.GetConnectionByKey(MakeConnKey("M", "old"), &gotOld);
  EXPECT_FALSE(oldStatus.ok());
  EXPECT_EQ(oldStatus.error_code(), grpc::StatusCode::NOT_FOUND);

  DataCenterProto::ConnectionInfo gotNew;
  ASSERT_TRUE(core.GetConnectionByKey(MakeConnKey("M", "new"), &gotNew).ok());
  EXPECT_EQ(gotNew.conn_id(), created.conn_id());

  DataCenterProto::GetOrCreateConnectionRequest getReq;
  *getReq.mutable_key() = MakeConnKey("M", "new");
  DataCenterProto::ConnectionInfo gotAgain;
  ASSERT_TRUE(core.GetOrCreateConnection(getReq, &gotAgain).ok());
  EXPECT_EQ(gotAgain.conn_id(), created.conn_id());
}

// 验证：UpsertConnection 在 key 已被其他 conn_id 占用时返回 ALREADY_EXISTS。
TEST(DataCenterCoreTest, UpsertConnectionReturnsAlreadyExistsWhenKeyBelongsToOtherConnId) {
  DataCenterCore core;

  DataCenterProto::GetOrCreateConnectionRequest c1;
  *c1.mutable_key() = MakeConnKey("M", "A");
  DataCenterProto::ConnectionInfo conn1;
  ASSERT_TRUE(core.GetOrCreateConnection(c1, &conn1).ok());

  DataCenterProto::GetOrCreateConnectionRequest c2;
  *c2.mutable_key() = MakeConnKey("M", "B");
  DataCenterProto::ConnectionInfo conn2;
  ASSERT_TRUE(core.GetOrCreateConnection(c2, &conn2).ok());

  DataCenterProto::UpsertConnectionRequest upsertReq;
  *upsertReq.mutable_conn() = MakeConnInfo(conn1.conn_id(), "M", "B");
  auto status = core.UpsertConnection(upsertReq);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：ListRoutes 支持按 src/dst 过滤。
TEST(DataCenterCoreTest, ListRoutesFiltersByFields) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2, 3});

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "A", 2, "B");
  *routes.add_routes() = MakeRoute(1, "C", 2, "D");
  *routes.add_routes() = MakeRoute(3, "A", 2, "B");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::ListRoutesRequest req;
  req.set_src_conn_id(1);
  req.set_dst_tag("B");
  auto resp = core.ListRoutes(req);
  ASSERT_EQ(resp.routes_size(), 1);
  EXPECT_EQ(resp.routes(0).src().conn_id(), 1u);
  EXPECT_EQ(resp.routes(0).src().tag(), "A");
  EXPECT_EQ(resp.routes(0).dst().conn_id(), 2u);
  EXPECT_EQ(resp.routes(0).dst().tag(), "B");
}

// 验证：DeleteRoutes 会删除指定路由，并使后续 Publish 不再产生更新。
TEST(DataCenterCoreTest, DeleteRoutesRemovesRouteAndStopsPublish) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "A", 2, "B");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::PublishRequest pub;
  pub.set_conn_id(1);
  pub.set_tag("A");
  pub.mutable_value()->set_int_value(1);

  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(core.Publish(pub, &updates).ok());
  ASSERT_EQ(updates.size(), 1u);

  DataCenterProto::DeleteRoutesRequest del;
  *del.add_routes() = MakeRoute(1, "A", 2, "B");
  ASSERT_TRUE(core.DeleteRoutes(del).ok());

  updates.clear();
  ASSERT_TRUE(core.Publish(pub, &updates).ok());
  EXPECT_TRUE(updates.empty());
}

// 验证：DeleteRoutes 校验失败时不会删除已经处理过的部分路由。
TEST(DataCenterCoreTest, DeleteRoutesFailureKeepsExistingRoutes) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "A", 2, "B");
  *routes.add_routes() = MakeRoute(1, "C", 2, "D");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::DeleteRoutesRequest del;
  *del.add_routes() = MakeRoute(1, "A", 2, "B");
  *del.add_routes() = MakeRoute(99, "不存在的连接", 2, "D");

  auto status = core.DeleteRoutes(del);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);

  DataCenterProto::ListRoutesRequest listReq;
  auto listResp = core.ListRoutes(listReq);
  ASSERT_EQ(listResp.routes_size(), 2);
  EXPECT_EQ(listResp.routes(0).src().tag(), "A");
  EXPECT_EQ(listResp.routes(0).dst().tag(), "B");
  EXPECT_EQ(listResp.routes(1).src().tag(), "C");
  EXPECT_EQ(listResp.routes(1).dst().tag(), "D");
}

// 验证：GetLatest 支持 tags 过滤，仅返回指定目的点的最新值。
TEST(DataCenterCoreTest, GetLatestFiltersByTags) {
  DataCenterCore core;
  InstallRouteConnections(core, {1, 2});

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "A", 2, "B");
  *routes.add_routes() = MakeRoute(1, "C", 2, "D");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  DataCenterProto::PublishRequest pubA;
  pubA.set_conn_id(1);
  pubA.set_tag("A");
  pubA.mutable_value()->set_int_value(10);
  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(core.Publish(pubA, &updates).ok());

  DataCenterProto::PublishRequest pubC;
  pubC.set_conn_id(1);
  pubC.set_tag("C");
  pubC.mutable_value()->set_int_value(20);
  ASSERT_TRUE(core.Publish(pubC, &updates).ok());

  DataCenterProto::GetLatestRequest req;
  req.set_conn_id(2);
  req.add_tags("D");
  DataCenterProto::GetLatestResponse resp;
  ASSERT_TRUE(core.GetLatest(req, &resp).ok());
  ASSERT_EQ(resp.updates_size(), 1);
  EXPECT_EQ(resp.updates(0).dst_tag(), "D");
  EXPECT_EQ(resp.updates(0).value().int_value(), 20);
}

// 验证：新的 DataCenterCore 实例拥有新的进程统计窗口，启动时间有效且计数清零。
TEST(DataCenterCoreTest, ThroughputSnapshotStartsFreshForNewCoreInstance) {
  DataCenterCore first;
  InstallRouteConnections(first, {1, 2});

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "A", 2, "B");
  ASSERT_TRUE(first.UpsertRoutes(routes).ok());

  DataCenterProto::PublishRequest publish;
  publish.set_conn_id(1);
  publish.set_tag("A");
  publish.mutable_value()->set_int_value(1);
  std::vector<DataCenterProto::PointUpdate> updates;
  ASSERT_TRUE(first.Publish(publish, &updates).ok());
  ASSERT_EQ(first.GetThroughputSnapshot().peak_points_per_second(), 1u);

  const auto secondSnapshot = DataCenterCore{}.GetThroughputSnapshot();
  EXPECT_GT(secondSnapshot.process_start_time_ms(), 0);
  EXPECT_EQ(secondSnapshot.current_points_per_second(), 0u);
  EXPECT_EQ(secondSnapshot.peak_points_per_second(), 0u);
  EXPECT_EQ(secondSnapshot.samples_size(), 0);
}
