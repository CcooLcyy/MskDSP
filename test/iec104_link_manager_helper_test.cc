#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <string>
#include <vector>

#define private public
#include "IEC104LinkManager.h"
#undef private

#include "IEC104ReportPolicy.hpp"
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
  p1->set_business_type(IEC104Proto::POINT_BUSINESS_TYPE_TELEMETRY);
  p1->set_scale(2.0);
  p1->set_offset(1.0);
  p1->set_deadband(0.5);

  auto* p3 = req.add_points();
  p3->set_tag("float-tag-2");
  p3->set_ioa(101);
  p3->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  p3->set_business_type(IEC104Proto::POINT_BUSINESS_TYPE_TELEMETRY);
  p3->set_scale(1.0);
  p3->set_offset(0.0);

  auto* p2 = req.add_points();
  p2->set_tag("single-tag");
  p2->set_ioa(200);
  p2->set_type(IEC104Proto::POINT_TYPE_SINGLE);
  p2->set_business_type(IEC104Proto::POINT_BUSINESS_TYPE_TELEINDICATION);

  auto* remoteAdjust = req.add_points();
  remoteAdjust->set_tag("remote-adjust-tag");
  remoteAdjust->set_ioa(102);
  remoteAdjust->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  remoteAdjust->set_business_type(IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_ADJUST);
  remoteAdjust->set_scale(2.0);
  remoteAdjust->set_offset(1.0);

  auto* remoteControl = req.add_points();
  remoteControl->set_tag("remote-control-tag");
  remoteControl->set_ioa(201);
  remoteControl->set_type(IEC104Proto::POINT_TYPE_SINGLE);
  remoteControl->set_business_type(IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_CONTROL);

  auto* doubleControl = req.add_points();
  doubleControl->set_tag("double-control-tag");
  doubleControl->set_ioa(202);
  doubleControl->set_type(IEC104Proto::POINT_TYPE_SINGLE);
  doubleControl->set_business_type(IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_CONTROL);
  doubleControl->set_remote_control_type(IEC104Proto::REMOTE_CONTROL_TYPE_DOUBLE);

  table.Upsert(req.points(), true);
  return table;
}

const IEC104Proto::SimulationPoint *FindSimulationPoint(
    const IEC104Proto::SimulationSnapshot &snapshot, const std::string &tag) {
  for (const auto &point : snapshot.points()) {
    if (point.tag() == tag) {
      return &point;
    }
  }
  return nullptr;
}
}  // 命名空间结束

// 验证：遥测数值未越过死区时，品质或时标变化仍会触发上报。
TEST(IEC104ReportPolicyTest, QualityOrTimestampChangeBypassesDeadband) {
  const IEC104::detail::LastReportedTelemetry last{
      .value = mskdsp::numeric::Decimal20::Parse("10").value(),
      .quality = DataCenterProto::QUALITY_GOOD,
      .tsMs = 100,
  };

  EXPECT_FALSE(IEC104::detail::ShouldReportTelemetry(10.1, 1.0, DataCenterProto::QUALITY_GOOD, 100, last));
  EXPECT_TRUE(IEC104::detail::ShouldReportTelemetry(10.1, 1.0, DataCenterProto::QUALITY_UNCERTAIN, 100, last));
  EXPECT_TRUE(IEC104::detail::ShouldReportTelemetry(10.1, 1.0, DataCenterProto::QUALITY_GOOD, 101, last));
  EXPECT_TRUE(IEC104::detail::ShouldReportTelemetry(11.0, 1.0, DataCenterProto::QUALITY_GOOD, 100, last));
}

// 验证：Decimal20 遥测值精确达到死区边界时会触发上报。
TEST(IEC104ReportPolicyTest, DecimalValueAtDeadbandBoundaryReports) {
  const IEC104::detail::LastReportedTelemetry last{
      .value = mskdsp::numeric::Decimal20::Parse(
                   "0.10000000000000000001")
                   .value(),
      .quality = DataCenterProto::QUALITY_GOOD,
      .tsMs = 100,
  };
  const auto current = mskdsp::numeric::Decimal20::Parse(
                           "0.30000000000000000001")
                           .value();
  const auto deadband = mskdsp::numeric::Decimal20::Parse("0.2").value();

  EXPECT_TRUE(IEC104::detail::ShouldReportTelemetry(
      current, deadband, DataCenterProto::QUALITY_GOOD, 100, last));
}

// 验证：遥信无历史基线时（例如链路启动后的第一条更新）必须形成 SOE 事件。
TEST(IEC104ReportPolicyTest, SinglePointFirstUpdateReports) {
  EXPECT_TRUE(IEC104::detail::ShouldReportSoe(false, DataCenterProto::QUALITY_GOOD, std::nullopt));
  EXPECT_TRUE(IEC104::detail::ShouldReportSoe(true, DataCenterProto::QUALITY_GOOD, std::nullopt));
}

// 验证：遥信状态与品质均未变化时不再重复形成 SOE 事件（仅时标变化不构成变位）。
TEST(IEC104ReportPolicyTest, SinglePointUnchangedStateAndQualityIsFiltered) {
  const IEC104::detail::LastReportedSinglePoint lastOff{
      .value = false,
      .quality = DataCenterProto::QUALITY_GOOD,
  };
  const IEC104::detail::LastReportedSinglePoint lastOn{
      .value = true,
      .quality = DataCenterProto::QUALITY_GOOD,
  };

  EXPECT_FALSE(IEC104::detail::ShouldReportSoe(false, DataCenterProto::QUALITY_GOOD, lastOff));
  EXPECT_FALSE(IEC104::detail::ShouldReportSoe(true, DataCenterProto::QUALITY_GOOD, lastOn));
}

// 验证：遥信状态变位（分↔合）时必须形成 SOE 事件。
TEST(IEC104ReportPolicyTest, SinglePointStateChangeReports) {
  const IEC104::detail::LastReportedSinglePoint lastOff{
      .value = false,
      .quality = DataCenterProto::QUALITY_GOOD,
  };
  const IEC104::detail::LastReportedSinglePoint lastOn{
      .value = true,
      .quality = DataCenterProto::QUALITY_GOOD,
  };

  EXPECT_TRUE(IEC104::detail::ShouldReportSoe(true, DataCenterProto::QUALITY_GOOD, lastOff));
  EXPECT_TRUE(IEC104::detail::ShouldReportSoe(false, DataCenterProto::QUALITY_GOOD, lastOn));
}

// 验证：遥信状态未变但品质变化（例如 GOOD→BAD/UNCERTAIN）时必须形成 SOE 事件。
TEST(IEC104ReportPolicyTest, SinglePointQualityChangeReports) {
  const IEC104::detail::LastReportedSinglePoint last{
      .value = true,
      .quality = DataCenterProto::QUALITY_GOOD,
  };

  EXPECT_TRUE(IEC104::detail::ShouldReportSoe(true, DataCenterProto::QUALITY_BAD, last));
  EXPECT_TRUE(IEC104::detail::ShouldReportSoe(true, DataCenterProto::QUALITY_UNCERTAIN, last));
}

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

// 验证：ROLE_SERVER 配置白名单主站 IP 时必须使用合法 IP 地址。
TEST(IEC104LinkManagerHelperTest, ValidateServerClientIpWhitelist) {
  IEC104Proto::LinkConfig config;
  config.set_conn_name("server");
  config.set_role(IEC104Proto::ROLE_SERVER);
  config.mutable_local()->set_ip("0.0.0.0");
  config.mutable_local()->set_port(2404);

  // 未配置 remote.ip 时保持兼容，允许服务端不启用来源限制。
  auto st = LinkManager::validateLinkConfig(config);
  EXPECT_TRUE(st.ok());

  config.mutable_remote()->set_ip("192.168.1.10");
  st = LinkManager::validateLinkConfig(config);
  EXPECT_TRUE(st.ok());

  config.mutable_remote()->set_ip("not-an-ip");
  st = LinkManager::validateLinkConfig(config);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  config.mutable_remote()->set_ip("0.0.0.0");
  st = LinkManager::validateLinkConfig(config);
  EXPECT_TRUE(st.ok());

  config.mutable_remote()->set_ip("::");
  st = LinkManager::validateLinkConfig(config);
  EXPECT_TRUE(st.ok());
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
  runtime.lastReportedByTag["float-tag"] =
      mskdsp::numeric::Decimal20::Parse("21.1").value();
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

// 验证：handleCommandValue 在非从站时忽略同步执行。
TEST(IEC104LinkManagerHelperTest, HandleCommandValueSkipsWhenNotSlave) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  LinkManager::LinkRuntime runtime;
  runtime.connId = 4;
  const auto connId = runtime.connId;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_MASTER);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  CommandValue cv;
  cv.ioa = 100;
  cv.type = IEC104Proto::POINT_TYPE_FLOAT;
  cv.doubleValue = 12.0;
  auto result = mgr.handleCommandValue("conn", cv);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(state.GetCommandCount(connId, "float-tag"), 0u);
}

// 验证：handleCommandValue 在从站下同步执行设点/遥控。
TEST(IEC104LinkManagerHelperTest, HandleCommandValueExecutesOnSlave) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  LinkManager::LinkRuntime runtime;
  runtime.connId = 5;
  const auto connId = runtime.connId;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  CommandValue cv;
  cv.ioa = 102;
  cv.type = IEC104Proto::POINT_TYPE_FLOAT;
  cv.doubleValue = 8.0;
  auto result = mgr.handleCommandValue("conn", cv);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(state.GetCommandCount(connId, "remote-adjust-tag"), 1u);

  cv.ioa = 201;
  cv.type = IEC104Proto::POINT_TYPE_SINGLE;
  cv.boolValue = true;
  result = mgr.handleCommandValue("conn", cv);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(state.GetCommandCount(connId, "remote-control-tag"), 1u);
}

// 验证：handleCommandValue 将双点遥控状态以 int64 1/2 传给 DataCenter。
TEST(IEC104LinkManagerHelperTest, HandleCommandValueMapsDoubleControlToInt64) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  EXPECT_CALL(*stub, ExecuteCommand(::testing::_, ::testing::_, ::testing::_))
      .WillOnce(::testing::Invoke([&state](grpc::ClientContext*,
                                          const DataCenterProto::ExecuteCommandRequest& request,
                                          DataCenterProto::ExecuteCommandResponse* response) {
        EXPECT_EQ(request.src().tag(), "double-control-tag");
        EXPECT_EQ(request.value().kind_case(), DataCenterProto::PointValue::kIntValue);
        EXPECT_EQ(request.value().int_value(), 2);
        return state.ExecuteCommand(request, response);
      }));

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  LinkManager::LinkRuntime runtime;
  runtime.connId = 51;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  CommandValue cv;
  cv.ioa = 202;
  cv.type = IEC104Proto::POINT_TYPE_SINGLE;
  cv.remoteControlType = IEC104Proto::REMOTE_CONTROL_TYPE_DOUBLE;
  cv.controlValue = 2;
  cv.boolValue = true;
  const auto result = mgr.handleCommandValue("conn", cv);
  EXPECT_TRUE(result.accepted);
}

// 验证：handleCommandValue 拒绝把遥信点当作遥控命令目标。
TEST(IEC104LinkManagerHelperTest, HandleCommandValueRejectsTeleindicationTarget) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  LinkManager::LinkRuntime runtime;
  runtime.connId = 52;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  CommandValue cv;
  cv.ioa = 200;
  cv.type = IEC104Proto::POINT_TYPE_SINGLE;
  cv.boolValue = true;
  const auto result = mgr.handleCommandValue("conn", cv);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(state.GetCommandCount(52, "single-tag"), 0u);
}

// 验证：handleCommandValue 会把 DataCenter 同步命令拒绝转换为业务拒绝结果。
TEST(IEC104LinkManagerHelperTest, HandleCommandValueReturnsRejectedWhenDataCenterRejects) {
  FakeDataCenterState state;
  state.RejectCommandForTag("remote-adjust-tag", "总量超过上限");
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  LinkManager::LinkRuntime runtime;
  runtime.connId = 6;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  CommandValue cv;
  cv.ioa = 102;
  cv.type = IEC104Proto::POINT_TYPE_FLOAT;
  cv.doubleValue = 8.0;
  auto result = mgr.handleCommandValue("conn", cv);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason, "总量超过上限");
}

// 验证：handleTimeSyncCommand 收到无效时间戳时不调用系统设时。
TEST(IEC104LinkManagerHelperTest, HandleTimeSyncCommandIgnoresInvalidTs) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  bool systemTimeSetterCalled = false;
  mgr.systemTimeSetter_ = [&systemTimeSetterCalled](int64_t) {
    systemTimeSetterCalled = true;
    return grpc::Status::OK;
  };

  LinkManager::LinkRuntime runtime;
  runtime.connId = 6;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  auto st = mgr.handleTimeSyncCommand("conn", 0);
  EXPECT_TRUE(st.ok());
  EXPECT_FALSE(systemTimeSetterCalled);
  EXPECT_EQ(state.GetPublishCount(6, "__time_sync__"), 0u);
}

// 验证：从站收到有效对时后无条件设置系统时间且不发布对时事件。
TEST(IEC104LinkManagerHelperTest, HandleTimeSyncCommandSetsSystemClockWithoutPublishingEvent) {
  FakeDataCenterState state;
  state.FailPublishForTag("__time_sync__");
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  int64_t requestedTsMs = 0;
  mgr.systemTimeSetter_ = [&requestedTsMs](int64_t tsMs) {
    requestedTsMs = tsMs;
    return grpc::Status::OK;
  };

  LinkManager::LinkRuntime runtime;
  runtime.connId = 8;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  auto st = mgr.handleTimeSyncCommand("conn", 1710000000123);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(requestedTsMs, 1710000000123);
  EXPECT_EQ(state.GetPublishCount(8, "__time_sync__"), 0u);
  EXPECT_EQ(state.GetFailedPublishCount(8, "__time_sync__"), 0u);
}

// 验证：系统设时失败时对时业务失败，且不发布任何对时事件。
TEST(IEC104LinkManagerHelperTest, HandleTimeSyncCommandRejectsSystemClockFailure) {
  FakeDataCenterState state;
  state.FailPublishForTag("__time_sync__");
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  int64_t requestedTsMs = 0;
  mgr.systemTimeSetter_ = [&requestedTsMs](int64_t tsMs) {
    requestedTsMs = tsMs;
    return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "测试设时权限不足");
  };

  LinkManager::LinkRuntime runtime;
  runtime.connId = 9;
  runtime.config = MakeClientConfig("conn", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  mgr.linksByName_.emplace("conn", std::move(runtime));

  auto st = mgr.handleTimeSyncCommand("conn", 1710000000123);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::PERMISSION_DENIED);
  EXPECT_EQ(requestedTsMs, 1710000000123);
  EXPECT_EQ(state.GetPublishCount(9, "__time_sync__"), 0u);
  EXPECT_EQ(state.GetFailedPublishCount(9, "__time_sync__"), 0u);
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

// 验证：无 DataCenter Route 时遥测按 IOA 升序连续递增，并且重复查询不会改变快照。
TEST(IEC104LinkManagerHelperTest, GeneratesAndKeepsSimulationValuesWithoutRoute) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  LinkManager::LinkRuntime runtime;
  runtime.config = MakeClientConfig("sim", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.config.set_role(IEC104Proto::ROLE_SERVER);
  runtime.pointTable = MakePointTable();
  runtime.pointTableConfigured = true;
  mgr.linksByName_.emplace("sim", std::move(runtime));

  IEC104Proto::SimulationSnapshot randomGenerated;
  IEC104Proto::SimulationRequest randomRequest;
  randomRequest.set_conn_name("sim");
  ASSERT_TRUE(mgr.GenerateSimulationValues(randomRequest, &randomGenerated).ok());
  ASSERT_EQ(randomGenerated.points_size(), 3);
  EXPECT_GE(randomGenerated.points(0).double_value(), 0.0);
  EXPECT_LE(randomGenerated.points(0).double_value(), 100.0);
  EXPECT_GE(randomGenerated.points(1).double_value(), 0.0);
  EXPECT_LE(randomGenerated.points(1).double_value(), 100.0);

  IEC104Proto::SimulationSnapshot generated;
  IEC104Proto::SimulationRequest request;
  request.set_conn_name("sim");
  request.set_mode(IEC104Proto::SIMULATION_MODE_INCREMENT);
  ASSERT_TRUE(mgr.GenerateSimulationValues(request, &generated).ok());
  ASSERT_EQ(generated.points_size(), 3);

  ASSERT_EQ(generated.points(0).tag(), "float-tag");
  ASSERT_DOUBLE_EQ(generated.points(0).double_value(), 1.0);
  ASSERT_EQ(generated.points(1).tag(), "float-tag-2");
  ASSERT_DOUBLE_EQ(generated.points(1).double_value(), 2.0);
  EXPECT_TRUE(generated.points(2).has_bool_value());

  // 标签顺序与 IOA 顺序相反时，递增值仍绑定 IOA 升序，而不是标签字典序。
  auto reorderedTable = MakePointTable();
  IEC104Proto::UpsertPointTableRequest replaceRequest;
  auto *latePoint = replaceRequest.add_points();
  latePoint->set_tag("a-telemetry");
  latePoint->set_ioa(4003);
  latePoint->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  auto *earlyPoint = replaceRequest.add_points();
  earlyPoint->set_tag("z-telemetry");
  earlyPoint->set_ioa(4001);
  earlyPoint->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  ASSERT_TRUE(reorderedTable.Upsert(replaceRequest.points(), true).ok());

  LinkManager::LinkRuntime reorderedRuntime;
  reorderedRuntime.config = MakeClientConfig("sim-reordered", IEC104Proto::STATION_ROLE_SLAVE);
  reorderedRuntime.config.set_role(IEC104Proto::ROLE_SERVER);
  reorderedRuntime.pointTable = std::move(reorderedTable);
  reorderedRuntime.pointTableConfigured = true;
  mgr.linksByName_.emplace("sim-reordered", std::move(reorderedRuntime));

  IEC104Proto::SimulationSnapshot reordered;
  IEC104Proto::SimulationRequest reorderedRequest;
  reorderedRequest.set_conn_name("sim-reordered");
  reorderedRequest.set_mode(IEC104Proto::SIMULATION_MODE_INCREMENT);
  ASSERT_TRUE(mgr.GenerateSimulationValues(reorderedRequest, &reordered).ok());
  ASSERT_EQ(reordered.points_size(), 2);
  EXPECT_EQ(reordered.points(0).tag(), "z-telemetry");
  EXPECT_DOUBLE_EQ(reordered.points(0).double_value(), 1.0);
  EXPECT_EQ(reordered.points(1).tag(), "a-telemetry");
  EXPECT_DOUBLE_EQ(reordered.points(1).double_value(), 2.0);

  IEC104Proto::SimulationSnapshot loaded;
  ASSERT_TRUE(mgr.GetSimulationSnapshot("sim", &loaded).ok());
  EXPECT_EQ(loaded.SerializeAsString(), generated.SerializeAsString());
}

// 验证：模拟快照只包含遥测和遥信，遥调、遥控点不会生成或发送模拟点值。
TEST(IEC104LinkManagerHelperTest, SimulationValuesExcludeRemoteAdjustAndRemoteControl) {
  LinkManager mgr("IEC104");
  LinkManager::LinkRuntime runtime;
  runtime.config = MakeClientConfig("sim-business", IEC104Proto::STATION_ROLE_SLAVE);
  auto table = MakePointTable();
  IEC104Proto::UpsertPointTableRequest commandPoints;
  auto* remoteAdjust = commandPoints.add_points();
  remoteAdjust->set_tag("remote-adjust");
  remoteAdjust->set_ioa(0x6201);
  remoteAdjust->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  remoteAdjust->set_business_type(IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_ADJUST);
  auto* remoteControl = commandPoints.add_points();
  remoteControl->set_tag("remote-control");
  remoteControl->set_ioa(0x6001);
  remoteControl->set_type(IEC104Proto::POINT_TYPE_SINGLE);
  remoteControl->set_business_type(IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_CONTROL);
  ASSERT_TRUE(table.Upsert(commandPoints.points(), false).ok());
  runtime.pointTable = std::move(table);
  runtime.pointTableConfigured = true;
  mgr.linksByName_.emplace("sim-business", std::move(runtime));

  IEC104Proto::SimulationSnapshot generated;
  IEC104Proto::SimulationRequest request;
  request.set_conn_name("sim-business");
  ASSERT_TRUE(mgr.GenerateSimulationValues(request, &generated).ok());
  EXPECT_EQ(generated.points_size(), 3);
  EXPECT_EQ(FindSimulationPoint(generated, "remote-adjust"), nullptr);
  EXPECT_EQ(FindSimulationPoint(generated, "remote-control"), nullptr);
}

// 验证：模拟值只遮蔽同 Tag 的真实数据，未模拟的遥调点仍保留在总召快照中。
TEST(IEC104LinkManagerHelperTest, InterrogationMergesSimulationWithUnsimulatedRealValues) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  LinkManager::LinkRuntime runtime;
  runtime.connId = 12;
  runtime.config = MakeClientConfig("sim-merge", IEC104Proto::STATION_ROLE_SLAVE);
  auto table = MakePointTable();
  IEC104Proto::UpsertPointTableRequest commandPoint;
  auto* remoteAdjust = commandPoint.add_points();
  remoteAdjust->set_tag("remote-adjust");
  remoteAdjust->set_ioa(0x6201);
  remoteAdjust->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  remoteAdjust->set_business_type(IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_ADJUST);
  ASSERT_TRUE(table.Upsert(commandPoint.points(), false).ok());
  runtime.pointTable = std::move(table);
  runtime.pointTableConfigured = true;
  IEC104Proto::SimulationPoint simulation;
  simulation.set_tag("float-tag");
  simulation.set_type(IEC104Proto::POINT_TYPE_FLOAT);
  simulation.set_double_value(11.0);
  runtime.simulationValues.emplace("float-tag", simulation);
  mgr.linksByName_.emplace("sim-merge", std::move(runtime));

  DataCenterProto::PublishRequest publish;
  publish.set_conn_id(12);
  publish.set_tag("remote-adjust");
  publish.mutable_value()->set_double_value(15.0);
  publish.set_quality(DataCenterProto::QUALITY_GOOD);
  ASSERT_TRUE(state.Publish(publish).ok());

  const auto snapshot = mgr.buildInterrogationSnapshot("sim-merge");
  ASSERT_EQ(snapshot.size(), 2u);
  EXPECT_EQ(snapshot[0].ioa, 100u);
  EXPECT_DOUBLE_EQ(snapshot[0].doubleValue, 5.0);
  EXPECT_EQ(snapshot[1].ioa, 0x6201u);
  EXPECT_DOUBLE_EQ(snapshot[1].doubleValue, 15.0);
}

// 验证：遥信模拟方式可以把所有 SINGLE 点统一置为 true 或 false。
TEST(IEC104LinkManagerHelperTest, SimulationValuesSetAllSingleValues) {
  LinkManager mgr("IEC104");
  LinkManager::LinkRuntime runtime;
  runtime.config = MakeClientConfig("sim-single", IEC104Proto::STATION_ROLE_SLAVE);
  auto table = MakePointTable();
  IEC104Proto::UpsertPointTableRequest secondSingleRequest;
  auto *secondSingle = secondSingleRequest.add_points();
  secondSingle->set_tag("single-tag-2");
  secondSingle->set_ioa(203);
  secondSingle->set_type(IEC104Proto::POINT_TYPE_SINGLE);
  ASSERT_TRUE(table.Upsert(secondSingleRequest.points(), false).ok());
  runtime.pointTable = std::move(table);
  runtime.pointTableConfigured = true;
  mgr.linksByName_.emplace("sim-single", std::move(runtime));

  IEC104Proto::SimulationSnapshot generated;
  IEC104Proto::SimulationRequest request;
  request.set_conn_name("sim-single");
  request.set_bool_mode(IEC104Proto::SIMULATION_BOOL_MODE_ALL_TRUE);
  ASSERT_TRUE(mgr.GenerateSimulationValues(request, &generated).ok());
  ASSERT_EQ(generated.points_size(), 4);
  const auto *single = FindSimulationPoint(generated, "single-tag");
  ASSERT_NE(single, nullptr);
  ASSERT_TRUE(single->has_bool_value());
  EXPECT_TRUE(single->bool_value());
  const auto *second = FindSimulationPoint(generated, "single-tag-2");
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(second->has_bool_value());
  EXPECT_TRUE(second->bool_value());

  request.set_bool_mode(IEC104Proto::SIMULATION_BOOL_MODE_ALL_FALSE);
  ASSERT_TRUE(mgr.GenerateSimulationValues(request, &generated).ok());
  ASSERT_EQ(generated.points_size(), 4);
  single = FindSimulationPoint(generated, "single-tag");
  ASSERT_NE(single, nullptr);
  ASSERT_TRUE(single->has_bool_value());
  EXPECT_FALSE(single->bool_value());
  second = FindSimulationPoint(generated, "single-tag-2");
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(second->has_bool_value());
  EXPECT_FALSE(second->bool_value());
}

// 验证：取反模式优先使用已有模拟快照中的遥信值。
TEST(IEC104LinkManagerHelperTest, SimulationValuesInvertExistingSnapshot) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  LinkManager::LinkRuntime runtime;
  runtime.connId = 9;
  runtime.config = MakeClientConfig("sim-invert-snapshot", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  runtime.pointTableConfigured = true;
  IEC104Proto::SimulationPoint existing;
  existing.set_tag("single-tag");
  existing.set_type(IEC104Proto::POINT_TYPE_SINGLE);
  existing.set_bool_value(true);
  runtime.simulationValues.emplace("single-tag", existing);
  mgr.linksByName_.emplace("sim-invert-snapshot", std::move(runtime));

  IEC104Proto::SimulationSnapshot generated;
  IEC104Proto::SimulationRequest request;
  request.set_conn_name("sim-invert-snapshot");
  request.set_bool_mode(IEC104Proto::SIMULATION_BOOL_MODE_INVERT_CURRENT);
  ASSERT_TRUE(mgr.GenerateSimulationValues(request, &generated).ok());
  ASSERT_EQ(generated.points_size(), 3);
  const auto *single = FindSimulationPoint(generated, "single-tag");
  ASSERT_NE(single, nullptr);
  ASSERT_TRUE(single->has_bool_value());
  EXPECT_FALSE(single->bool_value());
}

// 验证：没有模拟快照时，取反模式读取 DataCenter 当前遥信值。
TEST(IEC104LinkManagerHelperTest, SimulationValuesInvertDataCenterLatestValue) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  DataCenterProto::PublishRequest publish;
  publish.set_conn_id(10);
  publish.set_tag("single-tag");
  publish.mutable_value()->set_bool_value(false);
  publish.set_quality(DataCenterProto::QUALITY_GOOD);
  ASSERT_TRUE(state.Publish(publish).ok());

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  LinkManager::LinkRuntime runtime;
  runtime.connId = 10;
  runtime.config = MakeClientConfig("sim-invert-latest", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  runtime.pointTableConfigured = true;
  mgr.linksByName_.emplace("sim-invert-latest", std::move(runtime));

  IEC104Proto::SimulationSnapshot generated;
  IEC104Proto::SimulationRequest request;
  request.set_conn_name("sim-invert-latest");
  request.set_bool_mode(IEC104Proto::SIMULATION_BOOL_MODE_INVERT_CURRENT);
  ASSERT_TRUE(mgr.GenerateSimulationValues(request, &generated).ok());
  ASSERT_EQ(generated.points_size(), 3);
  const auto *single = FindSimulationPoint(generated, "single-tag");
  ASSERT_NE(single, nullptr);
  ASSERT_TRUE(single->has_bool_value());
  EXPECT_TRUE(single->bool_value());
}

// 验证：取反模式没有模拟值且 DataCenter 无当前值时返回错误并保持原快照为空。
TEST(IEC104LinkManagerHelperTest, SimulationValuesInvertRejectsMissingCurrentValue) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  LinkManager::LinkRuntime runtime;
  runtime.connId = 11;
  runtime.config = MakeClientConfig("sim-invert-missing", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  runtime.pointTableConfigured = true;
  mgr.linksByName_.emplace("sim-invert-missing", std::move(runtime));

  IEC104Proto::SimulationSnapshot generated;
  generated.set_conn_name("sentinel");
  IEC104Proto::SimulationRequest request;
  request.set_conn_name("sim-invert-missing");
  request.set_bool_mode(IEC104Proto::SIMULATION_BOOL_MODE_INVERT_CURRENT);
  const auto status = mgr.GenerateSimulationValues(request, &generated);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  IEC104Proto::SimulationSnapshot loaded;
  ASSERT_TRUE(mgr.GetSimulationSnapshot("sim-invert-missing", &loaded).ok());
  EXPECT_TRUE(loaded.points().empty());
}

// 验证：从站客户端可以生成、查询并清除固定模拟值。
TEST(IEC104LinkManagerHelperTest, SimulationValuesAllowSlaveClient) {
  LinkManager mgr("IEC104");
  LinkManager::LinkRuntime runtime;
  runtime.config = MakeClientConfig("slave-client", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.pointTable = MakePointTable();
  runtime.pointTableConfigured = true;
  mgr.linksByName_.emplace("slave-client", std::move(runtime));

  IEC104Proto::SimulationRequest request;
  request.set_conn_name("slave-client");
  request.set_mode(IEC104Proto::SIMULATION_MODE_INCREMENT);

  IEC104Proto::SimulationSnapshot generated;
  ASSERT_TRUE(mgr.GenerateSimulationValues(request, &generated).ok());
  ASSERT_EQ(generated.points_size(), 3);

  IEC104Proto::SimulationSnapshot loaded;
  ASSERT_TRUE(mgr.GetSimulationSnapshot("slave-client", &loaded).ok());
  EXPECT_EQ(loaded.SerializeAsString(), generated.SerializeAsString());

  ASSERT_TRUE(mgr.ClearSimulationValues("slave-client").ok());
  ASSERT_TRUE(mgr.GetSimulationSnapshot("slave-client", &loaded).ok());
  EXPECT_EQ(loaded.points_size(), 0);
}

// 验证：模拟接口拒绝主站链路。
TEST(IEC104LinkManagerHelperTest, SimulationValuesRejectMasterStation) {
  LinkManager mgr("IEC104");
  LinkManager::LinkRuntime runtime;
  runtime.config = MakeClientConfig("master", IEC104Proto::STATION_ROLE_MASTER);
  runtime.pointTable = MakePointTable();
  runtime.pointTableConfigured = true;
  mgr.linksByName_.emplace("master", std::move(runtime));

  IEC104Proto::SimulationSnapshot response;
  IEC104Proto::SimulationRequest request;
  request.set_conn_name("master");
  auto st = mgr.GenerateSimulationValues(request, &response);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：总召优先使用固定模拟值，不读取 DataCenter 最新值。
TEST(IEC104LinkManagerHelperTest, InterrogationUsesSimulationSnapshotFirst) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  LinkManager::LinkRuntime runtime;
  runtime.connId = 7;
  runtime.config = MakeClientConfig("sim", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.config.set_role(IEC104Proto::ROLE_SERVER);
  runtime.pointTable = MakePointTable();
  runtime.pointTableConfigured = true;
  IEC104Proto::SimulationPoint point;
  point.set_tag("float-tag");
  point.set_type(IEC104Proto::POINT_TYPE_FLOAT);
  point.set_double_value(11.0);
  runtime.simulationValues.emplace("float-tag", point);
  mgr.linksByName_.emplace("sim", std::move(runtime));

  state.FailGetLatestForConn(7);
  auto snapshot = mgr.buildInterrogationSnapshot("sim");
  ASSERT_EQ(snapshot.size(), 1u);
  EXPECT_EQ(snapshot.front().ioa, 100u);
  EXPECT_DOUBLE_EQ(snapshot.front().doubleValue, 5.0);
}

// 验证：清除模拟值后总召恢复 DataCenter 最新值路径。
TEST(IEC104LinkManagerHelperTest, ClearSimulationValuesRestoresDataCenterSnapshot) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  LinkManager::LinkRuntime runtime;
  runtime.connId = 8;
  runtime.config = MakeClientConfig("sim", IEC104Proto::STATION_ROLE_SLAVE);
  runtime.config.set_role(IEC104Proto::ROLE_SERVER);
  runtime.pointTable = MakePointTable();
  runtime.pointTableConfigured = true;
  IEC104Proto::SimulationPoint point;
  point.set_tag("float-tag");
  point.set_type(IEC104Proto::POINT_TYPE_FLOAT);
  point.set_double_value(11.0);
  runtime.simulationValues.emplace("float-tag", point);
  mgr.linksByName_.emplace("sim", std::move(runtime));

  DataCenterProto::PublishRequest publish;
  publish.set_conn_id(8);
  publish.set_tag("float-tag");
  publish.mutable_value()->set_double_value(7.0);
  publish.set_quality(DataCenterProto::QUALITY_GOOD);
  ASSERT_TRUE(state.Publish(publish).ok());
  ASSERT_TRUE(mgr.ClearSimulationValues("sim").ok());

  state.FailGetLatestForConn(8);
  EXPECT_TRUE(mgr.buildInterrogationSnapshot("sim").empty());
}
