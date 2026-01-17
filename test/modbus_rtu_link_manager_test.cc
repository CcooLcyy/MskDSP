#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <unordered_set>

#include "DataCenter_mock.grpc.pb.h"
#include "ModbusRTULinkManager.h"
#include "support/FakeDataCenter.hpp"

namespace {
using ModbusRTU::LinkManager;

using ::testing::_;
using ::testing::Return;

ModbusRTUProto::UpsertLinkRequest MakeLinkReq(const char* connName, const char* device, uint32_t baud, uint32_t slaveId) {
  ModbusRTUProto::UpsertLinkRequest req;
  auto* cfg = req.mutable_config();
  cfg->set_conn_name(connName);
  auto* serial = cfg->mutable_serial();
  serial->set_device(device);
  serial->set_baud_rate(baud);
  serial->set_data_bits(8);
  serial->set_parity(ModbusRTUProto::PARITY_NONE);
  serial->set_stop_bits(ModbusRTUProto::STOP_BITS_ONE);
  serial->set_read_timeout_ms(1000);
  cfg->set_slave_id(slaveId);
  req.set_create_only(true);
  return req;
}

ModbusRTUProto::UpsertLinkRequest MakeMinimalLinkReq(const char* connName, const char* device, uint32_t slaveId) {
  ModbusRTUProto::UpsertLinkRequest req;
  auto* cfg = req.mutable_config();
  cfg->set_conn_name(connName);
  auto* serial = cfg->mutable_serial();
  serial->set_device(device);
  cfg->set_slave_id(slaveId);
  return req;
}

ModbusRTUProto::Point MakeCoilPoint(const char* tag, uint32_t address) {
  ModbusRTUProto::Point p;
  p.set_tag(tag);
  p.set_function(ModbusRTUProto::FUNCTION_READ_COILS);
  p.set_address(address);
  p.set_type(ModbusRTUProto::DATA_TYPE_BOOL);
  return p;
}
}  // namespace

// 验证：create_only UpsertLink 会向 DataCenter 取/建 conn_id，并回填到 LinkInfo。
TEST(ModbusRtuLinkManagerTest, UpsertLinkCreateOnlyReturnsConnId) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);
  auto req = MakeLinkReq("conn-1", "/dev/ttyUSB0", 9600, 1);

  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
  EXPECT_NE(info.conn_id(), 0u);
  EXPECT_EQ(info.state(), ModbusRTUProto::LINK_STATE_STOPPED);
  EXPECT_EQ(info.config().conn_name(), "conn-1");
  EXPECT_TRUE(state.HasConnection("ModbusRTU", "conn-1"));
}

// 验证：当 DataCenter 已存在相同 (module_name, conn_name) 时，create_only UpsertLink 返回 ALREADY_EXISTS。
TEST(ModbusRtuLinkManagerTest, UpsertLinkCreateOnlyRejectsWhenDataCenterAlreadyHasKey) {
  FakeDataCenterState state;
  state.AddConnection(42, "ModbusRTU", "dup");
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);
  auto req = MakeLinkReq("dup", "/dev/ttyUSB0", 9600, 1);

  EXPECT_CALL(*stub, GetOrCreateConnection(_, _, _)).Times(0);

  ModbusRTUProto::LinkInfo info;
  auto st = mgr.UpsertLink(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：同一串口参数一致时允许多个从站配置。
TEST(ModbusRtuLinkManagerTest, UpsertLinkAllowsSharedSerialWithSameConfig) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  auto req1 = MakeLinkReq("conn-1", "/dev/ttyUSB0", 9600, 1);
  ModbusRTUProto::LinkInfo info1;
  ASSERT_TRUE(mgr.UpsertLink(req1, &info1).ok());

  auto req2 = MakeLinkReq("conn-2", "/dev/ttyUSB0", 9600, 2);
  ModbusRTUProto::LinkInfo info2;
  ASSERT_TRUE(mgr.UpsertLink(req2, &info2).ok());
}

// 验证：同一串口但参数不一致时 UpsertLink 拒绝配置。
TEST(ModbusRtuLinkManagerTest, UpsertLinkRejectsSerialConflict) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  auto req1 = MakeLinkReq("conn-1", "/dev/ttyUSB0", 9600, 1);
  ModbusRTUProto::LinkInfo info1;
  ASSERT_TRUE(mgr.UpsertLink(req1, &info1).ok());

  auto req2 = MakeLinkReq("conn-2", "/dev/ttyUSB0", 19200, 2);
  ModbusRTUProto::LinkInfo info2;
  auto st = mgr.UpsertLink(req2, &info2);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：当 DataCenter 删除失败时，DeleteLink 标记 PENDING_DELETE 且保留本地配置以便重试。
TEST(ModbusRtuLinkManagerTest, DeleteLinkFailureMarksPendingDeleteAndKeepsLocal) {
  FakeDataCenterState state;
  state.FailDeleteForConnName("conn-fail");
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);
  auto req = MakeLinkReq("conn-fail", "/dev/ttyUSB0", 9600, 1);

  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  auto st = mgr.DeleteLink("conn-fail");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);

  ModbusRTUProto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-fail", &got).ok());
  EXPECT_EQ(got.state(), ModbusRTUProto::LINK_STATE_PENDING_DELETE);
}

// 验证：address_base=ONE 且点表包含 address=0 时，StartLink 返回 INVALID_ARGUMENT。
TEST(ModbusRtuLinkManagerTest, StartLinkRejectsZeroAddressWhenBaseOne) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  auto req = MakeLinkReq("conn-addr", "/dev/ttyFAKE", 9600, 1);
  req.mutable_config()->set_address_base(ModbusRTUProto::ADDRESS_BASE_ONE);
  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-addr");
  auto* point = ptReq.add_points();
  point->set_tag("coil-0");
  point->set_function(ModbusRTUProto::FUNCTION_READ_COILS);
  point->set_address(0);
  point->set_type(ModbusRTUProto::DATA_TYPE_BOOL);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  auto st = mgr.StartLink("conn-addr");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-addr", &got).ok());
  EXPECT_EQ(got.state(), ModbusRTUProto::LINK_STATE_STOPPED);
}

// 验证：未设置 mode 时默认归一化为 MASTER。
TEST(ModbusRtuLinkManagerTest, UpsertLinkDefaultsToMasterMode) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);
  auto req = MakeLinkReq("conn-default-mode", "/dev/ttyUSB0", 9600, 1);

  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
  EXPECT_EQ(info.config().mode(), ModbusRTUProto::LINK_MODE_MASTER);
}

// 验证：同一串口不允许 MASTER/SLAVE 混用。
TEST(ModbusRtuLinkManagerTest, UpsertLinkRejectsMixedModesOnSameSerial) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  auto req1 = MakeLinkReq("conn-master", "/dev/ttyUSB0", 9600, 1);
  req1.mutable_config()->set_mode(ModbusRTUProto::LINK_MODE_MASTER);
  ModbusRTUProto::LinkInfo info1;
  ASSERT_TRUE(mgr.UpsertLink(req1, &info1).ok());

  auto req2 = MakeLinkReq("conn-slave", "/dev/ttyUSB0", 9600, 2);
  req2.mutable_config()->set_mode(ModbusRTUProto::LINK_MODE_SLAVE);
  ModbusRTUProto::LinkInfo info2;
  auto st = mgr.UpsertLink(req2, &info2);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：UpsertLink 会补齐默认串口与轮询参数。
TEST(ModbusRtuLinkManagerTest, UpsertLinkNormalizesDefaults) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  auto req = MakeMinimalLinkReq("conn-defaults", "/dev/ttyUSB0", 1);
  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  const auto& cfg = info.config();
  const auto& serial = cfg.serial();
  EXPECT_EQ(serial.baud_rate(), 9600u);
  EXPECT_EQ(serial.data_bits(), 8u);
  EXPECT_EQ(serial.parity(), ModbusRTUProto::PARITY_NONE);
  EXPECT_EQ(serial.stop_bits(), ModbusRTUProto::STOP_BITS_ONE);
  EXPECT_EQ(serial.read_timeout_ms(), 1000u);
  EXPECT_EQ(cfg.poll_interval_ms(), 1000u);
  EXPECT_EQ(cfg.address_base(), ModbusRTUProto::ADDRESS_BASE_ZERO);
  EXPECT_EQ(cfg.mode(), ModbusRTUProto::LINK_MODE_MASTER);
}

// 验证：串口 data_bits 越界时 UpsertLink 拒绝。
TEST(ModbusRtuLinkManagerTest, UpsertLinkRejectsInvalidDataBits) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  auto req = MakeMinimalLinkReq("conn-bad-bits", "/dev/ttyUSB0", 1);
  req.mutable_config()->mutable_serial()->set_data_bits(4);
  ModbusRTUProto::LinkInfo info;
  auto st = mgr.UpsertLink(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：slave_id 超出范围时 UpsertLink 拒绝。
TEST(ModbusRtuLinkManagerTest, UpsertLinkRejectsInvalidSlaveId) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  auto req = MakeMinimalLinkReq("conn-bad-id", "/dev/ttyUSB0", 0);
  ModbusRTUProto::LinkInfo info;
  auto st = mgr.UpsertLink(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：create_only=false 时可更新已有链接配置。
TEST(ModbusRtuLinkManagerTest, UpsertLinkUpdatesExistingConfig) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  auto req1 = MakeMinimalLinkReq("conn-update", "/dev/ttyUSB0", 1);
  ModbusRTUProto::LinkInfo info1;
  ASSERT_TRUE(mgr.UpsertLink(req1, &info1).ok());

  auto req2 = MakeMinimalLinkReq("conn-update", "/dev/ttyUSB0", 1);
  req2.mutable_config()->set_poll_interval_ms(2000);
  ModbusRTUProto::LinkInfo info2;
  ASSERT_TRUE(mgr.UpsertLink(req2, &info2).ok());
  EXPECT_EQ(info2.conn_id(), info1.conn_id());
  EXPECT_EQ(info2.config().poll_interval_ms(), 2000u);
}

// 验证：GetLink 与 ListLinks 能正确返回已配置的连接。
TEST(ModbusRtuLinkManagerTest, GetLinkAndListLinksReturnConfiguredLinks) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo info1;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-a", "/dev/ttyUSB0", 1), &info1).ok());
  ModbusRTUProto::LinkInfo info2;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-b", "/dev/ttyUSB1", 2), &info2).ok());

  ModbusRTUProto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-a", &got).ok());
  EXPECT_EQ(got.config().conn_name(), "conn-a");

  ModbusRTUProto::ListLinksResponse resp;
  ASSERT_TRUE(mgr.ListLinks(&resp).ok());
  EXPECT_EQ(resp.links_size(), 2);
  std::unordered_set<std::string> names;
  for (const auto& link : resp.links()) {
    names.insert(link.config().conn_name());
  }
  EXPECT_TRUE(names.contains("conn-a"));
  EXPECT_TRUE(names.contains("conn-b"));
}

// 验证：pending delete 状态会阻止 StartLink 与 UpsertPointTable。
TEST(ModbusRtuLinkManagerTest, PendingDeleteBlocksStartAndPointTableUpdate) {
  FakeDataCenterState state;
  state.FailDeleteForConnName("conn-pending");
  auto stub = MakeStub(&state);

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-pending", "/dev/ttyUSB0", 1), &info).ok());

  auto del = mgr.DeleteLink("conn-pending");
  EXPECT_EQ(del.error_code(), grpc::StatusCode::INTERNAL);

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-pending");
  *ptReq.add_points() = MakeCoilPoint("coil-1", 1);
  ptReq.set_replace(true);
  auto upsert = mgr.UpsertPointTable(ptReq);
  EXPECT_EQ(upsert.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  auto start = mgr.StartLink("conn-pending");
  EXPECT_EQ(start.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：DataCenter 更新失败时点表不落地到本地。
TEST(ModbusRtuLinkManagerTest, UpsertPointTableKeepsLocalOnDataCenterFailure) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  EXPECT_CALL(*stub, UpsertPointTable(_, _, _))
      .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "dc failure")));

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-pt", "/dev/ttyUSB0", 1), &info).ok());

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-pt");
  *ptReq.add_points() = MakeCoilPoint("coil-1", 1);
  ptReq.set_replace(true);
  auto st = mgr.UpsertPointTable(ptReq);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);

  ModbusRTUProto::PointTable out;
  ASSERT_TRUE(mgr.GetPointTable("conn-pt", &out).ok());
  EXPECT_EQ(out.points_size(), 0);
}
