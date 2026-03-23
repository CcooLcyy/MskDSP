#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <stdlib.h>
#include <string>
#include <unordered_set>
#include <unistd.h>

#include "DataCenter_mock.grpc.pb.h"
#include "ModbusRTULinkManager.h"
#include "support/FakeDataCenter.hpp"

namespace {
using ModbusRTU::LinkManager;

using ::testing::_;
using ::testing::HasSubstr;
using ::testing::Return;

ModbusRTUProto::UpsertLinkRequest MakeLinkReq(const char* connName, const char* device, uint32_t baud, uint32_t deviceId) {
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
  cfg->set_device_id(deviceId);
  req.set_create_only(true);
  return req;
}

ModbusRTUProto::UpsertLinkRequest MakeMinimalLinkReq(const char* connName, const char* device, uint32_t deviceId) {
  ModbusRTUProto::UpsertLinkRequest req;
  auto* cfg = req.mutable_config();
  cfg->set_conn_name(connName);
  auto* serial = cfg->mutable_serial();
  serial->set_device(device);
  cfg->set_device_id(deviceId);
  return req;
}

ModbusRTUProto::UpsertLinkRequest MakeMqttLinkReq(const char* connName, const char* serialPort, uint32_t deviceId) {
  ModbusRTUProto::UpsertLinkRequest req;
  auto* cfg = req.mutable_config();
  cfg->set_conn_name(connName);
  cfg->set_transport_type(ModbusRTUProto::TRANSPORT_MQTT_UART);
  cfg->set_serial_port(serialPort);
  auto* serial = cfg->mutable_serial();
  serial->set_baud_rate(9600);
  serial->set_data_bits(8);
  serial->set_parity(ModbusRTUProto::PARITY_NONE);
  serial->set_stop_bits(ModbusRTUProto::STOP_BITS_ONE);
  cfg->set_device_id(deviceId);
  return req;
}

ModbusRTUProto::UpdateConfigRequest MakeMqttUpdateRequest(const char* host, uint32_t port, const char* clientId) {
  ModbusRTUProto::UpdateConfigRequest req;
  auto* mqtt = req.mutable_mqtt();
  mqtt->set_host(host);
  mqtt->set_port(port);
  mqtt->set_client_id(clientId);
  mqtt->set_keepalive_sec(30);
  mqtt->set_clean_session(true);
  mqtt->set_connect_timeout_ms(3000);
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

ModbusRTUProto::Point MakeWriteSingleRegisterPoint(const char* tag, uint32_t address) {
  ModbusRTUProto::Point p;
  p.set_tag(tag);
  p.set_function(ModbusRTUProto::FUNCTION_WRITE_SINGLE_REGISTER);
  p.set_address(address);
  p.set_type(ModbusRTUProto::DATA_TYPE_UINT16);
  return p;
}

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    path_ = base / ("modbus_rtu_link_manager_test_tmp_" + std::to_string(ts) + "_" + std::to_string(counter_++));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

private:
  inline static uint64_t counter_ = 0;
  std::filesystem::path path_;
};

class ScopedPseudoTty {
public:
  ScopedPseudoTty() {
    masterFd_ = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (masterFd_ < 0) {
      error_ = std::string("打开 PTY 主端失败: ") + std::strerror(errno);
      return;
    }
    if (::grantpt(masterFd_) != 0) {
      error_ = std::string("授权 PTY 从端失败: ") + std::strerror(errno);
      closeAll();
      return;
    }
    if (::unlockpt(masterFd_) != 0) {
      error_ = std::string("解锁 PTY 从端失败: ") + std::strerror(errno);
      closeAll();
      return;
    }

    char slaveName[128] = {};
    if (::ptsname_r(masterFd_, slaveName, sizeof(slaveName)) != 0) {
      error_ = std::string("获取 PTY 从端路径失败: ") + std::strerror(errno);
      closeAll();
      return;
    }
    slavePath_ = slaveName;

    slaveFd_ = ::open(slavePath_.c_str(), O_RDWR | O_NOCTTY);
    if (slaveFd_ < 0) {
      error_ = std::string("打开 PTY 从端失败: ") + std::strerror(errno);
      closeAll();
      return;
    }
  }

  ~ScopedPseudoTty() { closeAll(); }

  bool ok() const { return !slavePath_.empty(); }
  const std::string& error() const { return error_; }
  const std::string& slavePath() const { return slavePath_; }

private:
  void closeAll() {
    if (slaveFd_ >= 0) {
      ::close(slaveFd_);
      slaveFd_ = -1;
    }
    if (masterFd_ >= 0) {
      ::close(masterFd_);
      masterFd_ = -1;
    }
  }

  int masterFd_ = -1;
  int slaveFd_ = -1;
  std::string slavePath_;
  std::string error_;
};

class LinkManagerTestEnv {
public:
  LinkManagerTestEnv() :
    mgr("ModbusRTU", dir.path() / "mqtt.pb", dir.path() / "links.pb", dir.path() / "point_tables.pb") {}

private:
  ScopedTempDir dir;

public:
  LinkManager mgr;
};
}  // 命名空间结束

// 验证：create_only UpsertLink 会向 DataCenter 取/建 conn_id，并回填到 LinkInfo。
TEST(ModbusRtuLinkManagerTest, UpsertLinkCreateOnlyReturnsConnId) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
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

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);
  auto req = MakeLinkReq("dup", "/dev/ttyUSB0", 9600, 1);

  EXPECT_CALL(*stub, GetOrCreateConnection(_, _, _)).Times(0);

  ModbusRTUProto::LinkInfo info;
  auto st = mgr.UpsertLink(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：同一串口参数一致时允许多个链路配置。
TEST(ModbusRtuLinkManagerTest, UpsertLinkAllowsSharedSerialWithSameConfig) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
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

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
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

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
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

// 验证：address_base=ONE 且点表包含 address=0 时，StartLink 返回 FAILED_PRECONDITION。
TEST(ModbusRtuLinkManagerTest, StartLinkRejectsZeroAddressWhenBaseOne) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
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
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  ModbusRTUProto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-addr", &got).ok());
  EXPECT_EQ(got.state(), ModbusRTUProto::LINK_STATE_STOPPED);
}

// 验证：UpsertLink 会补齐默认串口与轮询参数。
TEST(ModbusRtuLinkManagerTest, UpsertLinkNormalizesDefaults) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
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
}

// 验证：UpdateConfig 缺少 MQTT 配置时返回参数错误。
TEST(ModbusRtuLinkManagerTest, UpdateConfigRejectsMissingMqtt) {
  LinkManagerTestEnv env;
  auto& mgr = env.mgr;

  ModbusRTUProto::UpdateConfigRequest req;
  ModbusRTUProto::UpdateConfigResponse resp;
  auto st = mgr.UpdateConfig(req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(resp.ok());
}

// 验证：UpdateConfig 参数完整时返回成功。
TEST(ModbusRtuLinkManagerTest, UpdateConfigAcceptsValidMqtt) {
  LinkManagerTestEnv env;
  auto& mgr = env.mgr;

  auto req = MakeMqttUpdateRequest("127.0.0.1", 1883, "modbus-rtu-test");
  ModbusRTUProto::UpdateConfigResponse resp;
  auto st = mgr.UpdateConfig(req, &resp);
  EXPECT_TRUE(st.ok());
  EXPECT_TRUE(resp.ok());
}

// 验证：MQTT UART 链路会补齐默认远端串口超时参数。
TEST(ModbusRtuLinkManagerTest, UpsertLinkNormalizesMqttDefaults) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  auto req = MakeMqttLinkReq("conn-mqtt-defaults", "RS485-1", 1);
  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  const auto& cfg = info.config();
  EXPECT_EQ(cfg.transport_type(), ModbusRTUProto::TRANSPORT_MQTT_UART);
  EXPECT_EQ(cfg.serial_port(), "RS485-1");
  EXPECT_EQ(cfg.request_timeout_ms(), 3000u);
  EXPECT_EQ(cfg.serial_byte_timeout_ms(), 100u);
  EXPECT_EQ(cfg.serial_frame_timeout_ms(), 100u);
  EXPECT_EQ(cfg.serial_est_size(), 256u);
}

// 验证：未下发 MQTT 全局参数时，MQTT UART 链路不能启动连接功能。
TEST(ModbusRtuLinkManagerTest, StartLinkRejectsMqttWithoutUpdateConfig) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  auto req = MakeMqttLinkReq("conn-mqtt-start", "RS485-1", 1);
  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  auto st = mgr.StartLink("conn-mqtt-start");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：串口 data_bits 越界时 UpsertLink 拒绝。
TEST(ModbusRtuLinkManagerTest, UpsertLinkRejectsInvalidDataBits) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  auto req = MakeMinimalLinkReq("conn-bad-bits", "/dev/ttyUSB0", 1);
  req.mutable_config()->mutable_serial()->set_data_bits(4);
  ModbusRTUProto::LinkInfo info;
  auto st = mgr.UpsertLink(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：device_id 超出范围时 UpsertLink 拒绝。
TEST(ModbusRtuLinkManagerTest, UpsertLinkRejectsInvalidDeviceId) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
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

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
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

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
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

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
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

  EXPECT_CALL(*stub, UpsertConnTags(_, _, _))
      .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "dc failure")));

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
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

// 验证：UpdateConfig 成功后会将 MQTT 配置落盘到本地文件。
TEST(ModbusRtuLinkManagerTest, UpdateConfigPersistsMqttConfigToFile) {
  ScopedTempDir dir;
  const auto mqttPath = dir.path() / "mqtt.pb";
  const auto linksPath = dir.path() / "links.pb";
  const auto pointTablesPath = dir.path() / "point_tables.pb";

  LinkManager mgr("ModbusRTU", mqttPath, linksPath, pointTablesPath);

  auto req = MakeMqttUpdateRequest("127.0.0.1", 1883, "modbus-rtu-persist");
  ModbusRTUProto::UpdateConfigResponse resp;
  ASSERT_TRUE(mgr.UpdateConfig(req, &resp).ok());
  EXPECT_TRUE(resp.ok());
  EXPECT_TRUE(std::filesystem::exists(mqttPath));
}

// 验证：链路配置与点表在落盘后可被新 LinkManager 实例恢复，且恢复后会自动启动模块内连接功能。
TEST(ModbusRtuLinkManagerTest, LoadsPersistedLinkAndPointTableAfterRestart) {
  ScopedTempDir dir;
  ScopedPseudoTty pty;
  const auto mqttPath = dir.path() / "mqtt.pb";
  const auto linksPath = dir.path() / "links.pb";
  const auto pointTablesPath = dir.path() / "point_tables.pb";
  ASSERT_TRUE(pty.ok()) << pty.error();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  uint32_t connId = 0;
  {
    LinkManager mgr("ModbusRTU", mqttPath, linksPath, pointTablesPath);
    mgr.setDataCenterStub(stub);

    ModbusRTUProto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-persist", pty.slavePath().c_str(), 1), &info).ok());
    connId = info.conn_id();

    ModbusRTUProto::UpsertPointTableRequest ptReq;
    ptReq.set_conn_name("conn-persist");
    ptReq.set_replace(true);
    *ptReq.add_points() = MakeWriteSingleRegisterPoint("reg-write-a", 1);
    ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
    ASSERT_TRUE(mgr.StopLink("conn-persist").ok());
  }

  {
    LinkManager mgr("ModbusRTU", mqttPath, linksPath, pointTablesPath);
    mgr.setDataCenterStub(stub);
    mgr.LoadPersistedConfig();

    ModbusRTUProto::LinkInfo info;
    ASSERT_TRUE(mgr.GetLink("conn-persist", &info).ok());
    EXPECT_EQ(info.conn_id(), connId);
    EXPECT_EQ(info.state(), ModbusRTUProto::LINK_STATE_RUNNING);
    EXPECT_TRUE(info.last_error().empty());

    ModbusRTUProto::PointTable pointTable;
    ASSERT_TRUE(mgr.GetPointTable("conn-persist", &pointTable).ok());
    ASSERT_EQ(pointTable.points_size(), 1);
    EXPECT_EQ(pointTable.points(0).tag(), "reg-write-a");
    EXPECT_EQ(pointTable.points(0).address(), 1u);

    ASSERT_TRUE(mgr.StopLink("conn-persist").ok());
  }
}

// 验证：DeleteLink 进入 PENDING_DELETE 后会落盘，重启后仍阻止启动链路功能，并在 last_error 中反映阻塞原因。
TEST(ModbusRtuLinkManagerTest, LoadsPendingDeleteStateAfterRestart) {
  ScopedTempDir dir;
  const auto mqttPath = dir.path() / "mqtt.pb";
  const auto linksPath = dir.path() / "links.pb";
  const auto pointTablesPath = dir.path() / "point_tables.pb";

  FakeDataCenterState state;
  state.FailDeleteForConnName("conn-pending-persist");
  auto stub = MakeStub(&state);

  {
    LinkManager mgr("ModbusRTU", mqttPath, linksPath, pointTablesPath);
    mgr.setDataCenterStub(stub);

    ModbusRTUProto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-pending-persist", "/dev/ttyUSB0", 1), &info).ok());

    auto status = mgr.DeleteLink("conn-pending-persist");
    ASSERT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  }

  {
    LinkManager mgr("ModbusRTU", mqttPath, linksPath, pointTablesPath);
    mgr.setDataCenterStub(stub);
    mgr.LoadPersistedConfig();

    ModbusRTUProto::LinkInfo info;
    ASSERT_TRUE(mgr.GetLink("conn-pending-persist", &info).ok());
    EXPECT_EQ(info.state(), ModbusRTUProto::LINK_STATE_PENDING_DELETE);
    EXPECT_THAT(info.last_error(), HasSubstr("待删除状态"));

    auto status = mgr.StartLink("conn-pending-persist");
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  }
}
