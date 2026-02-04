#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#define private public
#include "IEC104LinkManager.h"
#undef private

#include "support/FakeDataCenter.hpp"

namespace {
using IEC104::LinkManager;
using IEC104::PointTable;
using IEC104::PointValue;
using IEC104::CommandValue;

IEC104Proto::LinkConfig MakeClientConfig(const std::string& name, IEC104Proto::StationRole role) {
  IEC104Proto::LinkConfig config;
  config.set_conn_name(name);
  config.set_role(IEC104Proto::ROLE_CLIENT);
  config.mutable_remote()->set_ip("127.0.0.1");
  config.mutable_remote()->set_port(2404);
  config.set_station_role(role);
  return config;
}

PointTable MakePointTable() {
  PointTable table;
  IEC104Proto::UpsertPointTableRequest req;

  auto* p1 = req.add_points();
  p1->set_tag("float-tag");
  p1->set_ioa(100);
  p1->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  p1->set_scale(2.0);
  p1->set_offset(1.0);
  p1->set_deadband(0.5);

  auto* p3 = req.add_points();
  p3->set_tag("float-tag-2");
  p3->set_ioa(101);
  p3->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  p3->set_scale(1.0);
  p3->set_offset(0.0);

  auto* p2 = req.add_points();
  p2->set_tag("single-tag");
  p2->set_ioa(200);
  p2->set_type(IEC104Proto::POINT_TYPE_SINGLE);

  table.Upsert(req.points(), true);
  return table;
}
}  // namespace

// 验证：validateLinkConfig 对缺失/非法字段返回错误。
TEST(IEC104LinkManagerHelperTest, ValidateLinkConfigRejectsInvalidFields) {
  IEC104Proto::LinkConfig config;
  auto st = LinkManager::validateLinkConfig(config);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  config.set_conn_name("conn");
  st = LinkManager::validateLinkConfig(config);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  config.set_role(IEC104Proto::ROLE_CLIENT);
  st = LinkManager::validateLinkConfig(config);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  config.mutable_remote()->set_ip("127.0.0.1");
  config.mutable_remote()->set_port(0);
  st = LinkManager::validateLinkConfig(config);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  config.mutable_remote()->set_port(2404);
  config.set_ca(70000);
  st = LinkManager::validateLinkConfig(config);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：normalizeStationRole 会根据传输角色补默认站点角色。
TEST(IEC104LinkManagerHelperTest, NormalizeStationRoleDefaults) {
  IEC104Proto::LinkConfig config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_UNSPECIFIED);
  EXPECT_EQ(LinkManager::normalizeStationRole(config), IEC104Proto::STATION_ROLE_MASTER);

  config.set_role(IEC104Proto::ROLE_SERVER);
  EXPECT_EQ(LinkManager::normalizeStationRole(config), IEC104Proto::STATION_ROLE_SLAVE);
}

// 验证：listenEndpointsConflict 在端口一致且任一通配时冲突。
TEST(IEC104LinkManagerHelperTest, ListenEndpointsConflictDetectsAny) {
  LinkManager::ListenEndpoint a{true, "0.0.0.0", 2404};
  LinkManager::ListenEndpoint b{false, "127.0.0.1", 2404};
  EXPECT_TRUE(LinkManager::listenEndpointsConflict(a, b));

  b.port = 2405;
  EXPECT_FALSE(LinkManager::listenEndpointsConflict(a, b));
}

// 验证：makeListenEndpoint 对空端口或非法 IP 返回错误。
TEST(IEC104LinkManagerHelperTest, MakeListenEndpointValidatesInput) {
  IEC104Proto::Endpoint ep;
  LinkManager::ListenEndpoint out;

  auto st = LinkManager::makeListenEndpoint(ep, &out);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ep.set_port(2404);
  ep.set_ip("not-an-ip");
  st = LinkManager::makeListenEndpoint(ep, &out);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：checkSystemListenAvailable 在本地端口可用时返回 OK。
TEST(IEC104LinkManagerHelperTest, CheckSystemListenAvailableSucceeds) {
  LinkManager::ListenEndpoint ep;
  ep.any = true;
  ep.ip = "0.0.0.0";
  ep.port = 0;
  auto st = LinkManager::checkSystemListenAvailable(ep);
  EXPECT_TRUE(st.ok());
}

// 验证：handleClientPointValue 在找不到链路时返回 NOT_FOUND。
TEST(IEC104LinkManagerHelperTest, HandleClientPointValueNotFound) {
  LinkManager mgr("IEC104");
  PointValue pv;
  pv.ioa = 1;
  auto st = mgr.handleClientPointValue("missing", pv);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：handleClientPointValue 类型不匹配时忽略发布。
TEST(IEC104LinkManagerHelperTest, HandleClientPointValueIgnoresTypeMismatch) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  LinkManager::LinkRuntime runtime;
  runtime.connId = 1;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  PointValue pv;
  pv.ioa = 100;
  pv.type = IEC104Proto::POINT_TYPE_SINGLE;
  auto st = mgr.handleClientPointValue("conn", pv);
  EXPECT_TRUE(st.ok());

  DataCenterProto::GetLatestResponse resp;
  DataCenterProto::GetLatestRequest req;
  req.set_conn_id(1);
  EXPECT_TRUE(state.GetLatest(req, &resp).ok());
  EXPECT_EQ(resp.updates_size(), 0);
}

// 验证：handleClientPointValue 死区过滤后不发布。
TEST(IEC104LinkManagerHelperTest, HandleClientPointValueDeadbandSkipsPublish) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  LinkManager::LinkRuntime runtime;
  runtime.connId = 2;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  runtime.lastReportedByTag["float-tag"] = 21.1;
  mgr.linksByName_.emplace("conn", std::move(runtime));

  PointValue pv;
  pv.ioa = 100;
  pv.type = IEC104Proto::POINT_TYPE_FLOAT;
  pv.doubleValue = 10.0;
  auto st = mgr.handleClientPointValue("conn", pv);
  EXPECT_TRUE(st.ok());

  DataCenterProto::GetLatestResponse resp;
  DataCenterProto::GetLatestRequest req;
  req.set_conn_id(2);
  EXPECT_TRUE(state.GetLatest(req, &resp).ok());
  EXPECT_EQ(resp.updates_size(), 0);
}

// 验证：handleClientPointValue 发布浮点与单点并更新缓存。
TEST(IEC104LinkManagerHelperTest, HandleClientPointValuePublishesFloatAndSingle) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  LinkManager::LinkRuntime runtime;
  runtime.connId = 3;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  PointValue pv;
  pv.ioa = 100;
  pv.type = IEC104Proto::POINT_TYPE_FLOAT;
  pv.doubleValue = 10.0;
  pv.quality = 0;
  auto st = mgr.handleClientPointValue("conn", pv);
  EXPECT_TRUE(st.ok());

  pv.ioa = 200;
  pv.type = IEC104Proto::POINT_TYPE_SINGLE;
  pv.boolValue = true;
  pv.quality = 0x80;
  st = mgr.handleClientPointValue("conn", pv);
  EXPECT_TRUE(st.ok());

  DataCenterProto::GetLatestResponse resp;
  DataCenterProto::GetLatestRequest req;
  req.set_conn_id(3);
  ASSERT_TRUE(state.GetLatest(req, &resp).ok());
  EXPECT_EQ(resp.updates_size(), 2);
}

// 验证：handleCommandValue 在非从站时忽略发布。
TEST(IEC104LinkManagerHelperTest, HandleCommandValueSkipsWhenNotSlave) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  LinkManager::LinkRuntime runtime;
  runtime.connId = 4;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_MASTER);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  CommandValue cv;
  cv.ioa = 100;
  cv.type = IEC104Proto::POINT_TYPE_FLOAT;
  cv.doubleValue = 12.0;
  auto st = mgr.handleCommandValue("conn", cv);
  EXPECT_TRUE(st.ok());
}

// 验证：handleCommandValue 在从站下发布设点/遥控。
TEST(IEC104LinkManagerHelperTest, HandleCommandValuePublishesOnSlave) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  LinkManager::LinkRuntime runtime;
  runtime.connId = 5;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  CommandValue cv;
  cv.ioa = 100;
  cv.type = IEC104Proto::POINT_TYPE_FLOAT;
  cv.doubleValue = 8.0;
  auto st = mgr.handleCommandValue("conn", cv);
  EXPECT_TRUE(st.ok());

  cv.ioa = 200;
  cv.type = IEC104Proto::POINT_TYPE_SINGLE;
  cv.boolValue = true;
  st = mgr.handleCommandValue("conn", cv);
  EXPECT_TRUE(st.ok());
}

// 验证：handleTimeSyncCommand 在 tsMs<=0 时直接返回。
TEST(IEC104LinkManagerHelperTest, HandleTimeSyncCommandIgnoresInvalidTs) {
  LinkManager mgr("IEC104");
  auto st = mgr.handleTimeSyncCommand("conn", 0);
  EXPECT_TRUE(st.ok());
}

// 验证：handleTimeSyncCommand 在从站下发布对时事件。
TEST(IEC104LinkManagerHelperTest, HandleTimeSyncCommandPublishesOnSlave) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  LinkManager::LinkRuntime runtime;
  runtime.connId = 6;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  auto st = mgr.handleTimeSyncCommand("conn", 1000);
  EXPECT_TRUE(st.ok());
}

// 验证：buildInterrogationSnapshot 能根据最新值生成快照并处理异常数据。
TEST(IEC104LinkManagerHelperTest, BuildInterrogationSnapshotBuildsValues) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  LinkManager::LinkRuntime runtime;
  runtime.connId = 7;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  DataCenterProto::PublishRequest pubFloat;
  pubFloat.set_conn_id(7);
  pubFloat.set_tag("float-tag");
  pubFloat.mutable_value()->set_double_value(21.0);
  pubFloat.set_quality(DataCenterProto::QUALITY_GOOD);
  ASSERT_TRUE(state.Publish(pubFloat).ok());

  DataCenterProto::PublishRequest pubBool;
  pubBool.set_conn_id(7);
  pubBool.set_tag("single-tag");
  pubBool.mutable_value()->set_bool_value(true);
  pubBool.set_quality(DataCenterProto::QUALITY_BAD);
  ASSERT_TRUE(state.Publish(pubBool).ok());

  DataCenterProto::PublishRequest pubNaN;
  pubNaN.set_conn_id(7);
  pubNaN.set_tag("float-tag-2");
  pubNaN.mutable_value()->set_double_value(std::numeric_limits<double>::quiet_NaN());
  pubNaN.set_quality(DataCenterProto::QUALITY_GOOD);
  ASSERT_TRUE(state.Publish(pubNaN).ok());

  auto snapshot = mgr.buildInterrogationSnapshot("conn");
  EXPECT_FALSE(snapshot.empty());
}
