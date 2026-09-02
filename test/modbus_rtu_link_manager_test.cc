#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <vector>

#include "DataCenter_mock.grpc.pb.h"
#include "ModbusRTULinkManager.h"
#include "ModbusRTUSerialBus.h"
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

ModbusRTUProto::Point MakeWriteSingleRegisterBoolPoint(const char* tag, uint32_t address) {
  ModbusRTUProto::Point p;
  p.set_tag(tag);
  p.set_function(ModbusRTUProto::FUNCTION_WRITE_SINGLE_REGISTER);
  p.set_address(address);
  p.set_type(ModbusRTUProto::DATA_TYPE_BOOL);
  return p;
}

ModbusRTUProto::Point MakeWriteSingleCoilPoint(const char* tag, uint32_t address) {
  ModbusRTUProto::Point p;
  p.set_tag(tag);
  p.set_function(ModbusRTUProto::FUNCTION_WRITE_SINGLE_COIL);
  p.set_address(address);
  p.set_type(ModbusRTUProto::DATA_TYPE_BOOL);
  return p;
}

ModbusRTUProto::Point MakeWriteMultipleRegistersPoint(const char* tag, uint32_t address) {
  ModbusRTUProto::Point p;
  p.set_tag(tag);
  p.set_function(ModbusRTUProto::FUNCTION_WRITE_MULTIPLE_REGISTERS);
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

  bool readExact(std::vector<uint8_t>* out, size_t size, int timeoutMs) const {
    if (out == nullptr || masterFd_ < 0) {
      return false;
    }
    out->clear();
    out->reserve(size);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (out->size() < size) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        return false;
      }
      pollfd fd{.fd = masterFd_, .events = POLLIN, .revents = 0};
      if (::poll(&fd, 1, static_cast<int>(remaining.count())) <= 0) {
        return false;
      }
      uint8_t buffer[64] = {};
      const auto toRead = std::min(size - out->size(), sizeof(buffer));
      const auto count = ::read(masterFd_, buffer, toRead);
      if (count <= 0) {
        return false;
      }
      out->insert(out->end(), buffer, buffer + count);
    }
    return true;
  }

  bool writeAll(const std::vector<uint8_t>& data) const {
    size_t offset = 0;
    while (offset < data.size()) {
      const auto count = ::write(masterFd_, data.data() + offset, data.size() - offset);
      if (count <= 0) {
        return false;
      }
      offset += static_cast<size_t>(count);
    }
    return true;
  }

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
    mgr("ModbusRTU", dir.path() / "config.db") {}

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

// 验证：逐点抄读模式可以保留合法显式区间配置，切换到区间模式时仍可复用。
TEST(ModbusRtuLinkManagerTest, UpsertLinkPointModePreservesReadBlocks) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  auto req = MakeMinimalLinkReq("conn-point-with-blocks", "/dev/ttyUSB0", 1);
  auto* plan = req.mutable_config()->mutable_read_plan();
  plan->set_mode(ModbusRTUProto::READ_PLAN_MODE_POINT);
  auto* block = plan->add_blocks();
  block->set_function(ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS);
  block->set_start(100);
  block->set_quantity(10);

  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
  ASSERT_TRUE(info.config().has_read_plan());
  EXPECT_EQ(info.config().read_plan().mode(), ModbusRTUProto::READ_PLAN_MODE_POINT);
  ASSERT_EQ(info.config().read_plan().blocks_size(), 1);
  EXPECT_EQ(info.config().read_plan().blocks(0).function(), ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS);
  EXPECT_EQ(info.config().read_plan().blocks(0).start(), 100u);
  EXPECT_EQ(info.config().read_plan().blocks(0).quantity(), 10u);

  ModbusRTUProto::LinkInfo saved;
  ASSERT_TRUE(mgr.GetLink("conn-point-with-blocks", &saved).ok());
  EXPECT_EQ(saved.config().read_plan().mode(), ModbusRTUProto::READ_PLAN_MODE_POINT);
  ASSERT_EQ(saved.config().read_plan().blocks_size(), 1);
  EXPECT_EQ(saved.config().read_plan().blocks(0).start(), 100u);
  EXPECT_EQ(saved.config().read_plan().blocks(0).quantity(), 10u);

  req.mutable_config()->mutable_read_plan()->set_mode(ModbusRTUProto::READ_PLAN_MODE_EXPLICIT);
  ModbusRTUProto::LinkInfo updated;
  ASSERT_TRUE(mgr.UpsertLink(req, &updated).ok());
  EXPECT_EQ(updated.config().read_plan().mode(), ModbusRTUProto::READ_PLAN_MODE_EXPLICIT);
  ASSERT_EQ(updated.config().read_plan().blocks_size(), 1);
  EXPECT_EQ(updated.config().read_plan().blocks(0).start(), 100u);
  EXPECT_EQ(updated.config().read_plan().blocks(0).quantity(), 10u);
}

// 验证：逐点抄读模式仅保留合法区间，quantity=0 的无效区间仍会被拒绝。
TEST(ModbusRtuLinkManagerTest, UpsertLinkPointModeRejectsInvalidReadBlock) {
  LinkManagerTestEnv env;
  auto& mgr = env.mgr;

  auto req = MakeMinimalLinkReq("conn-point-with-invalid-block", "/dev/ttyUSB0", 1);
  auto* plan = req.mutable_config()->mutable_read_plan();
  plan->set_mode(ModbusRTUProto::READ_PLAN_MODE_POINT);
  auto* block = plan->add_blocks();
  block->set_function(ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS);
  block->set_start(100);

  ModbusRTUProto::LinkInfo info;
  const auto status = mgr.UpsertLink(req, &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_THAT(status.error_message(), HasSubstr("read_plan.blocks.quantity"));
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

// 验证：未配置 MQTT 时查询返回 configured=false，且不会把空配置当作有效配置。
TEST(ModbusRtuLinkManagerTest, GetConfigReportsMissingMqtt) {
  LinkManagerTestEnv env;
  ModbusRTUProto::GetConfigResponse resp;

  const auto status = env.mgr.GetConfig(&resp);
  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(resp.configured());
  EXPECT_THAT(resp.message(), HasSubstr("MQTT 配置未配置"));
  EXPECT_FALSE(resp.has_mqtt());
}

// 验证：UpdateConfig 成功后 GetConfig 能回读当前 MQTT 配置的全部字段。
TEST(ModbusRtuLinkManagerTest, GetConfigReturnsCurrentMqtt) {
  LinkManagerTestEnv env;
  auto update = MakeMqttUpdateRequest("mqtt.example", 1884, "modbus-get-config");
  update.mutable_mqtt()->set_username("user");
  update.mutable_mqtt()->set_password("secret");

  ModbusRTUProto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(env.mgr.UpdateConfig(update, &updateResp).ok());

  ModbusRTUProto::GetConfigResponse resp;
  const auto status = env.mgr.GetConfig(&resp);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(resp.configured());
  ASSERT_TRUE(resp.has_mqtt());
  EXPECT_EQ(resp.mqtt().host(), "mqtt.example");
  EXPECT_EQ(resp.mqtt().port(), 1884u);
  EXPECT_EQ(resp.mqtt().client_id(), "modbus-get-config");
  EXPECT_EQ(resp.mqtt().username(), "user");
  EXPECT_EQ(resp.mqtt().password(), "secret");
  EXPECT_EQ(resp.mqtt().keepalive_sec(), 30u);
  EXPECT_TRUE(resp.mqtt().clean_session());
  EXPECT_EQ(resp.mqtt().connect_timeout_ms(), 3000u);
}

// 验证：GetConfig 能从本地 SQLite 持久化配置恢复当前 MQTT 参数。
TEST(ModbusRtuLinkManagerTest, GetConfigReturnsPersistedMqttAfterReload) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";
  const auto update = MakeMqttUpdateRequest("persisted.example", 1885, "modbus-persisted");
  {
    LinkManager mgr("ModbusRTU", configDbPath);
    ModbusRTUProto::UpdateConfigResponse updateResp;
    ASSERT_TRUE(mgr.UpdateConfig(update, &updateResp).ok());
  }

  LinkManager reloaded("ModbusRTU", configDbPath);
  reloaded.LoadPersistedConfig();
  ModbusRTUProto::GetConfigResponse resp;
  ASSERT_TRUE(reloaded.GetConfig(&resp).ok());
  ASSERT_TRUE(resp.configured());
  ASSERT_TRUE(resp.has_mqtt());
  EXPECT_EQ(resp.mqtt().host(), "persisted.example");
  EXPECT_EQ(resp.mqtt().port(), 1885u);
  EXPECT_EQ(resp.mqtt().client_id(), "modbus-persisted");
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

// 验证：UpsertPointTable 成功后链路保持 STOPPED，需显式调用 StartLink 才启动链路功能。
TEST(ModbusRtuLinkManagerTest, UpsertPointTableKeepsStoppedUntilExplicitStart) {
  ScopedPseudoTty pty;
  ASSERT_TRUE(pty.ok()) << pty.error();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-start-explicit", pty.slavePath().c_str(), 1), &info).ok());

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-start-explicit");
  *ptReq.add_points() = MakeWriteSingleRegisterPoint("reg-a", 1);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  ModbusRTUProto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-start-explicit", &got).ok());
  EXPECT_EQ(got.state(), ModbusRTUProto::LINK_STATE_STOPPED);
  EXPECT_TRUE(got.last_error().empty());

  ASSERT_TRUE(mgr.StartLink("conn-start-explicit").ok());
  ASSERT_TRUE(mgr.GetLink("conn-start-explicit", &got).ok());
  EXPECT_EQ(got.state(), ModbusRTUProto::LINK_STATE_RUNNING);
  EXPECT_TRUE(got.last_error().empty());

  ASSERT_TRUE(mgr.StopLink("conn-start-explicit").ok());
}

// 验证：停止态且点表已就绪的链路执行 UpsertLink 更新后，仍保持 STOPPED。
TEST(ModbusRtuLinkManagerTest, UpsertLinkUpdateKeepsStoppedWhenPointTableReady) {
  ScopedPseudoTty pty;
  ASSERT_TRUE(pty.ok()) << pty.error();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo created;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-update-ready", pty.slavePath().c_str(), 1), &created).ok());

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-update-ready");
  *ptReq.add_points() = MakeWriteSingleRegisterPoint("reg-a", 1);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  ASSERT_TRUE(mgr.StopLink("conn-update-ready").ok());

  auto updateReq = MakeMinimalLinkReq("conn-update-ready", pty.slavePath().c_str(), 1);
  updateReq.mutable_config()->set_poll_interval_ms(2000);
  ModbusRTUProto::LinkInfo updated;
  ASSERT_TRUE(mgr.UpsertLink(updateReq, &updated).ok());
  EXPECT_EQ(updated.config().poll_interval_ms(), 2000u);

  ModbusRTUProto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-update-ready", &got).ok());
  EXPECT_EQ(got.state(), ModbusRTUProto::LINK_STATE_STOPPED);
  EXPECT_TRUE(got.last_error().empty());
}

// 验证：UpdateConfig 成功后不会自动启动已具备运行条件的停止态链路。
TEST(ModbusRtuLinkManagerTest, UpdateConfigKeepsStoppedWhenReadyLinkExists) {
  ScopedPseudoTty pty;
  ASSERT_TRUE(pty.ok()) << pty.error();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-update-config", pty.slavePath().c_str(), 1), &info).ok());

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-update-config");
  *ptReq.add_points() = MakeWriteSingleRegisterPoint("reg-a", 1);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  ASSERT_TRUE(mgr.StopLink("conn-update-config").ok());

  auto req = MakeMqttUpdateRequest("127.0.0.1", 1883, "modbus-rtu-test");
  ModbusRTUProto::UpdateConfigResponse resp;
  ASSERT_TRUE(mgr.UpdateConfig(req, &resp).ok());
  EXPECT_TRUE(resp.ok());

  ModbusRTUProto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-update-config", &got).ok());
  EXPECT_EQ(got.state(), ModbusRTUProto::LINK_STATE_STOPPED);
  EXPECT_TRUE(got.last_error().empty());
}

// 验证：同步命令会按目的连接与写点发送 0x10 报文，并在收到合法从站响应后返回已接受。
TEST(ModbusRtuLinkManagerTest, ExecuteCommandWritesMultipleRegistersAndReturnsAccepted) {
  ScopedPseudoTty pty;
  ASSERT_TRUE(pty.ok()) << pty.error();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-command", pty.slavePath().c_str(), 1), &info).ok());

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-command");
  *ptReq.add_points() = MakeWriteMultipleRegistersPoint("active-power-setpoint", 100);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  ASSERT_TRUE(mgr.StartLink("conn-command").ok());

  std::vector<uint8_t> requestFrame;
  bool responseWritten = false;
  std::jthread responder([&]() {
    if (!pty.readExact(&requestFrame, 11, 3000)) {
      return;
    }
    std::vector<uint8_t> responseFrame(requestFrame.begin(), requestFrame.begin() + 6);
    ModbusRTU::SerialBus::appendCrc(&responseFrame);
    responseWritten = pty.writeAll(responseFrame);
  });

  DataCenterProto::ExecuteCommandRequest request;
  request.mutable_src()->set_conn_id(99);
  request.mutable_src()->set_tag("iec104-setpoint");
  request.mutable_dst()->set_conn_id(info.conn_id());
  request.mutable_dst()->set_module_name("ModbusRTU");
  request.mutable_dst()->set_conn_name("conn-command");
  request.mutable_dst()->set_tag("active-power-setpoint");
  request.mutable_value()->set_double_value(10.0);
  request.set_quality(DataCenterProto::QUALITY_GOOD);

  DataCenterProto::ExecuteCommandResponse response;
  auto status = mgr.ExecuteCommand(request, &response);
  responder.join();

  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(responseWritten);
  ASSERT_EQ(requestFrame.size(), 11u);
  EXPECT_EQ(requestFrame[0], 0x01);
  EXPECT_EQ(requestFrame[1], 0x10);
  EXPECT_EQ(requestFrame[2], 0x00);
  EXPECT_EQ(requestFrame[3], 0x64);
  EXPECT_EQ(requestFrame[4], 0x00);
  EXPECT_EQ(requestFrame[5], 0x01);
  EXPECT_EQ(requestFrame[6], 0x02);
  EXPECT_EQ(requestFrame[7], 0x00);
  EXPECT_EQ(requestFrame[8], 0x0A);
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_ACCEPTED);
  EXPECT_EQ(response.dst().conn_id(), info.conn_id());
  EXPECT_DOUBLE_EQ(response.requested_value(), 10.0);
  EXPECT_DOUBLE_EQ(response.accepted_value(), 10.0);

  ASSERT_TRUE(mgr.StopLink("conn-command").ok());
}

// 验证：同步 BOOL 命令通过 0x05 写单线圈，并按标准发送 true=FF00、false=0000。
TEST(ModbusRtuLinkManagerTest, ExecuteCommandWritesSingleCoilBoolOnAndOff) {
  ScopedPseudoTty pty;
  ASSERT_TRUE(pty.ok()) << pty.error();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-coil-command", pty.slavePath().c_str(), 1), &info).ok());

  ModbusRTUProto::UpsertPointTableRequest pointRequest;
  pointRequest.set_conn_name("conn-coil-command");
  *pointRequest.add_points() = MakeWriteSingleCoilPoint("coil-command", 10);
  pointRequest.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(pointRequest).ok());
  ASSERT_TRUE(mgr.StartLink("conn-coil-command").ok());

  std::vector<std::vector<uint8_t>> requestFrames(2);
  std::jthread responder([&]() {
    for (auto& requestFrame : requestFrames) {
      if (!pty.readExact(&requestFrame, 8, 3000)) {
        return;
      }
      std::vector<uint8_t> responseFrame(requestFrame.begin(), requestFrame.begin() + 6);
      ModbusRTU::SerialBus::appendCrc(&responseFrame);
      if (!pty.writeAll(responseFrame)) {
        return;
      }
    }
  });

  DataCenterProto::ExecuteCommandRequest onRequest;
  onRequest.mutable_src()->set_conn_id(99);
  onRequest.mutable_src()->set_tag("control-source");
  onRequest.mutable_dst()->set_conn_id(info.conn_id());
  onRequest.mutable_dst()->set_module_name("ModbusRTU");
  onRequest.mutable_dst()->set_conn_name("conn-coil-command");
  onRequest.mutable_dst()->set_tag("coil-command");
  onRequest.mutable_value()->set_bool_value(true);
  onRequest.set_quality(DataCenterProto::QUALITY_GOOD);
  DataCenterProto::ExecuteCommandResponse onResponse;
  ASSERT_TRUE(mgr.ExecuteCommand(onRequest, &onResponse).ok());
  EXPECT_EQ(onResponse.status(), DataCenterProto::COMMAND_ACCEPTED);

  DataCenterProto::ExecuteCommandRequest offRequest = onRequest;
  offRequest.mutable_value()->set_bool_value(false);
  DataCenterProto::ExecuteCommandResponse offResponse;
  ASSERT_TRUE(mgr.ExecuteCommand(offRequest, &offResponse).ok());
  EXPECT_EQ(offResponse.status(), DataCenterProto::COMMAND_ACCEPTED);
  responder.join();

  ASSERT_EQ(requestFrames.size(), 2u);
  ASSERT_EQ(requestFrames[0].size(), 8u);
  EXPECT_EQ(requestFrames[0][0], 0x01);
  EXPECT_EQ(requestFrames[0][1], 0x05);
  EXPECT_EQ(requestFrames[0][2], 0x00);
  EXPECT_EQ(requestFrames[0][3], 0x0A);
  EXPECT_EQ(requestFrames[0][4], 0xFF);
  EXPECT_EQ(requestFrames[0][5], 0x00);
  ASSERT_EQ(requestFrames[1].size(), 8u);
  EXPECT_EQ(requestFrames[1][0], 0x01);
  EXPECT_EQ(requestFrames[1][1], 0x05);
  EXPECT_EQ(requestFrames[1][2], 0x00);
  EXPECT_EQ(requestFrames[1][3], 0x0A);
  EXPECT_EQ(requestFrames[1][4], 0x00);
  EXPECT_EQ(requestFrames[1][5], 0x00);

  ASSERT_TRUE(mgr.StopLink("conn-coil-command").ok());
}

// 验证：同步 BOOL 命令通过 0x06 写单寄存器，并按工程约定发送寄存器值 1/0。
TEST(ModbusRtuLinkManagerTest, ExecuteCommandWritesSingleRegisterBoolAsOneOrZero) {
  ScopedPseudoTty pty;
  ASSERT_TRUE(pty.ok()) << pty.error();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-register-bool-command", pty.slavePath().c_str(), 1), &info).ok());

  ModbusRTUProto::UpsertPointTableRequest pointRequest;
  pointRequest.set_conn_name("conn-register-bool-command");
  *pointRequest.add_points() = MakeWriteSingleRegisterBoolPoint("register-command", 20);
  pointRequest.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(pointRequest).ok());
  ASSERT_TRUE(mgr.StartLink("conn-register-bool-command").ok());

  std::vector<std::vector<uint8_t>> requestFrames(2);
  std::jthread responder([&]() {
    for (auto& requestFrame : requestFrames) {
      if (!pty.readExact(&requestFrame, 8, 3000)) {
        return;
      }
      std::vector<uint8_t> responseFrame(requestFrame.begin(), requestFrame.begin() + 6);
      ModbusRTU::SerialBus::appendCrc(&responseFrame);
      if (!pty.writeAll(responseFrame)) {
        return;
      }
    }
  });

  DataCenterProto::ExecuteCommandRequest onRequest;
  onRequest.mutable_src()->set_conn_id(99);
  onRequest.mutable_src()->set_tag("control-source");
  onRequest.mutable_dst()->set_conn_id(info.conn_id());
  onRequest.mutable_dst()->set_module_name("ModbusRTU");
  onRequest.mutable_dst()->set_conn_name("conn-register-bool-command");
  onRequest.mutable_dst()->set_tag("register-command");
  onRequest.mutable_value()->set_bool_value(true);
  onRequest.set_quality(DataCenterProto::QUALITY_GOOD);
  DataCenterProto::ExecuteCommandResponse onResponse;
  ASSERT_TRUE(mgr.ExecuteCommand(onRequest, &onResponse).ok());
  EXPECT_EQ(onResponse.status(), DataCenterProto::COMMAND_ACCEPTED);

  DataCenterProto::ExecuteCommandRequest offRequest = onRequest;
  offRequest.mutable_value()->set_bool_value(false);
  DataCenterProto::ExecuteCommandResponse offResponse;
  ASSERT_TRUE(mgr.ExecuteCommand(offRequest, &offResponse).ok());
  EXPECT_EQ(offResponse.status(), DataCenterProto::COMMAND_ACCEPTED);
  responder.join();

  ASSERT_EQ(requestFrames.size(), 2u);
  ASSERT_EQ(requestFrames[0].size(), 8u);
  EXPECT_EQ(requestFrames[0][0], 0x01);
  EXPECT_EQ(requestFrames[0][1], 0x06);
  EXPECT_EQ(requestFrames[0][2], 0x00);
  EXPECT_EQ(requestFrames[0][3], 0x14);
  EXPECT_EQ(requestFrames[0][4], 0x00);
  EXPECT_EQ(requestFrames[0][5], 0x01);
  ASSERT_EQ(requestFrames[1].size(), 8u);
  EXPECT_EQ(requestFrames[1][0], 0x01);
  EXPECT_EQ(requestFrames[1][1], 0x06);
  EXPECT_EQ(requestFrames[1][2], 0x00);
  EXPECT_EQ(requestFrames[1][3], 0x14);
  EXPECT_EQ(requestFrames[1][4], 0x00);
  EXPECT_EQ(requestFrames[1][5], 0x00);

  ASSERT_TRUE(mgr.StopLink("conn-register-bool-command").ok());
}

// 验证：目的 ModbusRTU 链路未运行时，同步命令返回目标不可用且不会尝试写串口。
TEST(ModbusRtuLinkManagerTest, ExecuteCommandRejectsStoppedLink) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-stopped-command", "/dev/tty-not-opened", 1), &info).ok());

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-stopped-command");
  *ptReq.add_points() = MakeWriteSingleRegisterPoint("active-power-setpoint", 100);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  DataCenterProto::ExecuteCommandRequest request;
  request.mutable_dst()->set_conn_id(info.conn_id());
  request.mutable_dst()->set_module_name("ModbusRTU");
  request.mutable_dst()->set_conn_name("conn-stopped-command");
  request.mutable_dst()->set_tag("active-power-setpoint");
  request.mutable_value()->set_double_value(10.0);

  DataCenterProto::ExecuteCommandResponse response;
  auto status = mgr.ExecuteCommand(request, &response);

  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
  EXPECT_THAT(response.reason(), HasSubstr("链路未运行"));
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

// 验证：RenameLink 成功后保留 conn_id，旧名字失效，新名字可继续操作点表与启停。
TEST(ModbusRtuLinkManagerTest, RenameLinkKeepsConnIdAndMovesPointTable) {
  ScopedPseudoTty pty;
  ASSERT_TRUE(pty.ok()) << pty.error();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo created;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-old", pty.slavePath().c_str(), 1), &created).ok());

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-old");
  *ptReq.add_points() = MakeWriteSingleRegisterPoint("reg-a", 1);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  ASSERT_TRUE(mgr.StopLink("conn-old").ok());

  ModbusRTUProto::LinkInfo renamed;
  ASSERT_TRUE(mgr.RenameLink("conn-old", "conn-new", &renamed).ok());
  EXPECT_EQ(renamed.conn_id(), created.conn_id());
  EXPECT_EQ(renamed.config().conn_name(), "conn-new");
  EXPECT_FALSE(state.HasConnection("ModbusRTU", "conn-old"));
  EXPECT_TRUE(state.HasConnection("ModbusRTU", "conn-new"));

  ModbusRTUProto::LinkInfo oldInfo;
  auto oldStatus = mgr.GetLink("conn-old", &oldInfo);
  EXPECT_EQ(oldStatus.error_code(), grpc::StatusCode::NOT_FOUND);

  ModbusRTUProto::ListLinksResponse listResp;
  ASSERT_TRUE(mgr.ListLinks(&listResp).ok());
  ASSERT_EQ(listResp.links_size(), 1);
  EXPECT_EQ(listResp.links(0).config().conn_name(), "conn-new");

  ModbusRTUProto::PointTable pointTable;
  ASSERT_TRUE(mgr.GetPointTable("conn-new", &pointTable).ok());
  ASSERT_EQ(pointTable.points_size(), 1);
  EXPECT_EQ(pointTable.points(0).tag(), "reg-a");

  ModbusRTUProto::UpsertPointTableRequest ptUpdateReq;
  ptUpdateReq.set_conn_name("conn-new");
  *ptUpdateReq.add_points() = MakeCoilPoint("coil-b", 2);
  ptUpdateReq.set_replace(false);
  ASSERT_TRUE(mgr.UpsertPointTable(ptUpdateReq).ok());

  ASSERT_TRUE(mgr.GetPointTable("conn-new", &pointTable).ok());
  ASSERT_EQ(pointTable.points_size(), 2);
  std::unordered_set<std::string> pointTags;
  for (const auto& point : pointTable.points()) {
    pointTags.insert(point.tag());
  }
  EXPECT_EQ(pointTags, (std::unordered_set<std::string>{"reg-a", "coil-b"}));

  ASSERT_TRUE(mgr.StartLink("conn-new").ok());
  ASSERT_TRUE(mgr.StopLink("conn-new").ok());
  ASSERT_TRUE(mgr.DeleteLink("conn-new").ok());
  EXPECT_FALSE(state.HasConnection("ModbusRTU", "conn-new"));
}

// 验证：RenameLink 在目标名字已存在时返回 ALREADY_EXISTS。
TEST(ModbusRtuLinkManagerTest, RenameLinkRejectsWhenNewConnNameExists) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo oldInfo;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-old", "/dev/ttyUSB0", 1), &oldInfo).ok());
  ModbusRTUProto::LinkInfo newInfo;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-new", "/dev/ttyUSB1", 2), &newInfo).ok());

  ModbusRTUProto::LinkInfo renamed;
  auto status = mgr.RenameLink("conn-old", "conn-new", &renamed);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：RenameLink 在旧名字不存在时返回 NOT_FOUND。
TEST(ModbusRtuLinkManagerTest, RenameLinkRejectsWhenOldConnNameMissing) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo info;
  auto status = mgr.RenameLink("missing", "conn-new", &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：运行中的链路不允许 RenameLink。
TEST(ModbusRtuLinkManagerTest, RenameLinkRejectsWhenLinkRunning) {
  ScopedPseudoTty pty;
  ASSERT_TRUE(pty.ok()) << pty.error();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManagerTestEnv env;
  auto& mgr = env.mgr;
  mgr.setDataCenterStub(stub);

  ModbusRTUProto::LinkInfo created;
  ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-running-rename", pty.slavePath().c_str(), 1), &created).ok());

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-running-rename");
  *ptReq.add_points() = MakeWriteSingleRegisterPoint("reg-a", 1);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  ASSERT_TRUE(mgr.StartLink("conn-running-rename").ok());

  ModbusRTUProto::LinkInfo renamed;
  auto status = mgr.RenameLink("conn-running-rename", "conn-renamed", &renamed);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  ASSERT_TRUE(mgr.StopLink("conn-running-rename").ok());
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

// 验证：UpdateConfig 成功后会将 MQTT 配置落盘到 SQLite。
TEST(ModbusRtuLinkManagerTest, UpdateConfigPersistsMqttConfigToSqliteOnly) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";

  LinkManager mgr("ModbusRTU", configDbPath);

  auto req = MakeMqttUpdateRequest("127.0.0.1", 1883, "modbus-rtu-persist");
  ModbusRTUProto::UpdateConfigResponse resp;
  ASSERT_TRUE(mgr.UpdateConfig(req, &resp).ok());
  EXPECT_TRUE(resp.ok());
  EXPECT_TRUE(std::filesystem::exists(configDbPath));

  LinkManager reloaded("ModbusRTU", configDbPath);
  reloaded.LoadPersistedConfig();
}

// 验证：持久化恢复时 DataCenter 不可用，不会把已有点表配置回写为空。
TEST(ModbusRtuLinkManagerTest, LoadPersistedConfigKeepsPointTablesWhenDataCenterUnavailable) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";

  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  {
    LinkManager mgr("ModbusRTU", configDbPath);
    mgr.setDataCenterStub(stub);

    ModbusRTUProto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-dc-down", "/dev/ttyFAKE", 1), &info).ok());

    ModbusRTUProto::UpsertPointTableRequest ptReq;
    ptReq.set_conn_name("conn-dc-down");
    ptReq.set_replace(true);
    *ptReq.add_points() = MakeCoilPoint("coil-keep", 1);
    ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  }

  auto failingStub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  EXPECT_CALL(*failingStub, GetOrCreateConnection(_, _, _))
      .WillRepeatedly(Return(grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 未就绪")));
  {
    LinkManager mgr("ModbusRTU", configDbPath);
    mgr.setDataCenterStub(failingStub);
    mgr.LoadPersistedConfig();
  }

  {
    LinkManager mgr("ModbusRTU", configDbPath);
    mgr.setDataCenterStub(stub);
    mgr.LoadPersistedConfig();

    ModbusRTUProto::PointTable pointTable;
    ASSERT_TRUE(mgr.GetPointTable("conn-dc-down", &pointTable).ok());
    ASSERT_EQ(pointTable.points_size(), 1);
    EXPECT_EQ(pointTable.points(0).tag(), "coil-keep");
  }
}

// 验证：链路配置与点表在落盘后可被新 LinkManager 实例恢复，且恢复后会自动启动模块内连接功能。
TEST(ModbusRtuLinkManagerTest, LoadsPersistedLinkAndPointTableAfterRestart) {
  ScopedTempDir dir;
  ScopedPseudoTty pty;
  const auto configDbPath = dir.path() / "config.db";
  ASSERT_TRUE(pty.ok()) << pty.error();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  uint32_t connId = 0;
  {
    LinkManager mgr("ModbusRTU", configDbPath);
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
    LinkManager mgr("ModbusRTU", configDbPath);
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

// 验证：RenameLink 落盘后，新实例恢复时仅保留新名字且点表仍可读取。
TEST(ModbusRtuLinkManagerTest, LoadsRenamedLinkAfterRestart) {
  ScopedTempDir dir;
  ScopedPseudoTty pty;
  const auto configDbPath = dir.path() / "config.db";
  ASSERT_TRUE(pty.ok()) << pty.error();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  uint32_t connId = 0;
  {
    LinkManager mgr("ModbusRTU", configDbPath);
    mgr.setDataCenterStub(stub);

    ModbusRTUProto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-old-persist", pty.slavePath().c_str(), 1), &info).ok());
    connId = info.conn_id();

    ModbusRTUProto::UpsertPointTableRequest ptReq;
    ptReq.set_conn_name("conn-old-persist");
    ptReq.set_replace(true);
    *ptReq.add_points() = MakeWriteSingleRegisterPoint("reg-write-a", 1);
    ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
    ASSERT_TRUE(mgr.RenameLink("conn-old-persist", "conn-new-persist", &info).ok());
    ASSERT_TRUE(mgr.StopLink("conn-new-persist").ok());
  }

  {
    LinkManager mgr("ModbusRTU", configDbPath);
    mgr.setDataCenterStub(stub);
    mgr.LoadPersistedConfig();

    ModbusRTUProto::LinkInfo info;
    auto oldStatus = mgr.GetLink("conn-old-persist", &info);
    EXPECT_EQ(oldStatus.error_code(), grpc::StatusCode::NOT_FOUND);

    ASSERT_TRUE(mgr.GetLink("conn-new-persist", &info).ok());
    EXPECT_EQ(info.conn_id(), connId);

    ModbusRTUProto::PointTable pointTable;
    ASSERT_TRUE(mgr.GetPointTable("conn-new-persist", &pointTable).ok());
    ASSERT_EQ(pointTable.points_size(), 1);
    EXPECT_EQ(pointTable.points(0).tag(), "reg-write-a");

    ASSERT_TRUE(mgr.StopLink("conn-new-persist").ok());
  }
}

// 验证：DeleteLink 进入 PENDING_DELETE 后会落盘，重启后仍阻止启动链路功能，并在 last_error 中反映阻塞原因。
TEST(ModbusRtuLinkManagerTest, LoadsPendingDeleteStateAfterRestart) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";

  FakeDataCenterState state;
  state.FailDeleteForConnName("conn-pending-persist");
  auto stub = MakeStub(&state);

  {
    LinkManager mgr("ModbusRTU", configDbPath);
    mgr.setDataCenterStub(stub);

    ModbusRTUProto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(MakeMinimalLinkReq("conn-pending-persist", "/dev/ttyUSB0", 1), &info).ok());

    auto status = mgr.DeleteLink("conn-pending-persist");
    ASSERT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  }

  {
    LinkManager mgr("ModbusRTU", configDbPath);
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
