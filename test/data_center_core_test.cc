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
}  // namespace

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
  *createReq.mutable_key() = MakeConnKey("Modbus", "mb-1");

  DataCenterProto::ConnectionInfo created;
  ASSERT_TRUE(core.GetOrCreateConnection(createReq, &created).ok());

  DataCenterProto::RenameConnectionRequest renameReq;
  *renameReq.mutable_old_key() = MakeConnKey("Modbus", "mb-1");
  *renameReq.mutable_new_key() = MakeConnKey("Modbus", "mb-rename");

  DataCenterProto::ConnectionInfo renamed;
  ASSERT_TRUE(core.RenameConnection(renameReq, &renamed).ok());
  EXPECT_EQ(renamed.conn_id(), created.conn_id());
  EXPECT_EQ(renamed.module_name(), "Modbus");
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

// 验证：DeleteConnection 会清理该连接关联的点表/路由/最新值缓存。
TEST(DataCenterCoreTest, DeleteConnectionCleansPointTableRoutesAndLatest) {
  DataCenterCore core;

  DataCenterProto::GetOrCreateConnectionRequest createSrc;
  *createSrc.mutable_key() = MakeConnKey("Modbus", "mb-src");
  DataCenterProto::ConnectionInfo src;
  ASSERT_TRUE(core.GetOrCreateConnection(createSrc, &src).ok());

  DataCenterProto::GetOrCreateConnectionRequest createDst;
  *createDst.mutable_key() = MakeConnKey("IEC104", "104-dst");
  DataCenterProto::ConnectionInfo dst;
  ASSERT_TRUE(core.GetOrCreateConnection(createDst, &dst).ok());

  DataCenterProto::UpsertPointTableRequest srcPt;
  srcPt.set_conn_id(src.conn_id());
  srcPt.set_replace(true);
  srcPt.add_tags("A");
  ASSERT_TRUE(core.UpsertPointTable(srcPt).ok());

  DataCenterProto::UpsertPointTableRequest dstPt;
  dstPt.set_conn_id(dst.conn_id());
  dstPt.set_replace(true);
  dstPt.add_tags("B");
  ASSERT_TRUE(core.UpsertPointTable(dstPt).ok());

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

  DataCenterProto::DeleteConnectionRequest delReq;
  *delReq.mutable_key() = createSrc.key();
  ASSERT_TRUE(core.DeleteConnection(delReq).ok());

  DataCenterProto::PointTable table;
  auto ptStatus = core.GetPointTable(src.conn_id(), &table);
  EXPECT_FALSE(ptStatus.ok());
  EXPECT_EQ(ptStatus.error_code(), grpc::StatusCode::NOT_FOUND);

  DataCenterProto::ListRoutesRequest listReq;
  auto listResp = core.ListRoutes(listReq);
  EXPECT_EQ(listResp.routes_size(), 0);

  DataCenterProto::GetLatestResponse afterDel;
  ASSERT_TRUE(core.GetLatest(latestReq, &afterDel).ok());
  EXPECT_EQ(afterDel.updates_size(), 0);
}

// 验证：当点表存在时，UpsertRoutes 会校验 tag 必须在点表内。
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

// 验证：Publish 按路由进行 tag 重写（srcTag -> dstTag）的一对一转发。
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

// 验证：Publish 支持一对多路由，生成多个目的端点的更新。
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

// 验证：GetLatest 返回目标连接内“按目的端点”最新一次路由后的值（按 dst_tag 排序）。
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

// 验证：BatchPublish 在输入合法时会发布全部点并生成对应路由更新。
TEST(DataCenterCoreTest, BatchPublishPublishesAllPointsWhenValid) {
  DataCenterCore core;

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
}

// 验证：BatchPublish 在校验失败时具有原子性（不输出 updates 且不更新 latest）。
TEST(DataCenterCoreTest, BatchPublishIsAtomicAndDoesNotUpdateLatestOnValidationFailure) {
  DataCenterCore core;

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
}

// 验证：DumpPointTablesConfig 与 ReplacePointTablesConfig 可 roundtrip 恢复点表配置。
TEST(DataCenterCoreTest, DumpAndReplacePointTablesConfigRoundtrip) {
  DataCenterCore core;

  DataCenterProto::UpsertPointTableRequest pt;
  pt.set_conn_id(1);
  pt.set_replace(true);
  pt.add_tags("点1");
  pt.add_tags("点2");
  ASSERT_TRUE(core.UpsertPointTable(pt).ok());

  auto config = core.DumpPointTablesConfig();

  DataCenterCore restored;
  ASSERT_TRUE(restored.ReplacePointTablesConfig(config).ok());

  DataCenterProto::PointTable table;
  ASSERT_TRUE(restored.GetPointTable(1, &table).ok());
  ASSERT_EQ(table.tags_size(), 2);
  EXPECT_EQ(table.tags(0), "点1");
  EXPECT_EQ(table.tags(1), "点2");
}

// 验证：ReplacePointTablesConfig 会按 conn_id 合并并去重 tags。
TEST(DataCenterCoreTest, ReplacePointTablesConfigMergesAndDeduplicatesByConnId) {
  DataCenterCore core;

  DataCenterProto::PointTablesConfig config;
  auto* t1 = config.add_point_tables();
  t1->set_conn_id(1);
  t1->add_tags("A");
  t1->add_tags("B");

  auto* t2 = config.add_point_tables();
  t2->set_conn_id(1);
  t2->add_tags("B");
  t2->add_tags("C");

  ASSERT_TRUE(core.ReplacePointTablesConfig(config).ok());

  DataCenterProto::PointTable table;
  ASSERT_TRUE(core.GetPointTable(1, &table).ok());
  ASSERT_EQ(table.tags_size(), 3);
  EXPECT_EQ(table.tags(0), "A");
  EXPECT_EQ(table.tags(1), "B");
  EXPECT_EQ(table.tags(2), "C");
}

// 验证：DumpRoutesConfig 与 ReplaceRoutesConfig 可 roundtrip 恢复路由配置。
TEST(DataCenterCoreTest, DumpAndReplaceRoutesConfigRoundtrip) {
  DataCenterCore core;

  DataCenterProto::UpsertRoutesRequest routes;
  routes.set_replace(true);
  *routes.add_routes() = MakeRoute(1, "源点", 2, "目的点");
  *routes.add_routes() = MakeRoute(1, "源点", 3, "目的点2");
  ASSERT_TRUE(core.UpsertRoutes(routes).ok());

  auto config = core.DumpRoutesConfig();

  DataCenterCore restored;
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

// 验证：ReplaceRoutesConfig 会按 (src,dst) 对路由去重。
TEST(DataCenterCoreTest, ReplaceRoutesConfigDeduplicatesBySrcDst) {
  DataCenterCore core;

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

// 验证：当点表存在时，ReplaceRoutesConfig 会校验 tag 必须在点表内。
TEST(DataCenterCoreTest, ReplaceRoutesConfigValidatesAgainstPointTableWhenPresent) {
  DataCenterCore core;

  DataCenterProto::UpsertPointTableRequest pt;
  pt.set_conn_id(1);
  pt.set_replace(true);
  pt.add_tags("存在的点");
  ASSERT_TRUE(core.UpsertPointTable(pt).ok());

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

// 验证：GetLatest 支持 tags 过滤，仅返回指定目的点的最新值。
TEST(DataCenterCoreTest, GetLatestFiltersByTags) {
  DataCenterCore core;

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
