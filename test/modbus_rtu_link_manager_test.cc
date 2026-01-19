#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include "DataCenter_mock.grpc.pb.h"
#include "ModbusRTULinkManager.h"
#include "ModbusRTUSerialBus.h"
#include "support/FakeDataCenter.hpp"

namespace {
using ModbusRTU::LinkManager;
using ModbusRTU::SerialBus;

using ::testing::_;
using ::testing::Invoke;
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

ModbusRTUProto::Point MakeRegisterPoint(const char* tag, uint32_t address) {
  ModbusRTUProto::Point p;
  p.set_tag(tag);
  p.set_function(ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS);
  p.set_address(address);
  p.set_type(ModbusRTUProto::DATA_TYPE_UINT16);
  return p;
}

struct PtyPair {
  int master_fd = -1;
  std::string slave_path;
};

void CloseFd(int* fd) {
  if (fd == nullptr || *fd < 0) {
    return;
  }
  ::close(*fd);
  *fd = -1;
}

bool SetRawMode(int fd) {
  termios tio{};
  if (::tcgetattr(fd, &tio) != 0) {
    return false;
  }
  ::cfmakeraw(&tio);
  return ::tcsetattr(fd, TCSANOW, &tio) == 0;
}

PtyPair CreatePtyPair() {
  PtyPair pair;
  pair.master_fd = ::posix_openpt(O_RDWR | O_NOCTTY);
  if (pair.master_fd < 0) {
    return pair;
  }
  if (::grantpt(pair.master_fd) != 0 || ::unlockpt(pair.master_fd) != 0) {
    CloseFd(&pair.master_fd);
    return pair;
  }
  char* slave_name = ::ptsname(pair.master_fd);
  if (slave_name == nullptr) {
    CloseFd(&pair.master_fd);
    return pair;
  }
  pair.slave_path = slave_name;
  int slave_fd = ::open(slave_name, O_RDWR | O_NOCTTY);
  if (slave_fd < 0) {
    CloseFd(&pair.master_fd);
    pair.slave_path.clear();
    return pair;
  }
  SetRawMode(slave_fd);
  ::close(slave_fd);
  return pair;
}

bool WriteAll(int fd, const std::vector<uint8_t>& data) {
  size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t n = ::write(fd, data.data() + offset, data.size() - offset);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    offset += static_cast<size_t>(n);
  }
  return true;
}

bool ReadExact(int fd, uint8_t* out, size_t len, std::chrono::milliseconds timeout) {
  size_t offset = 0;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (offset < len) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd pfd{fd, POLLIN, 0};
    const int rc = ::poll(&pfd, 1, static_cast<int>(remaining));
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (rc == 0) {
      return false;
    }
    const ssize_t n = ::read(fd, out + offset, len - offset);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    offset += static_cast<size_t>(n);
  }
  return true;
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

// 验证：从站 0x03 在 DataCenter 无值时使用 default_uint16 兜底返回。
TEST(ModbusRtuLinkManagerTest, SlaveHoldingRegistersUsesDefaultWhenDataCenterEmpty) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  EXPECT_CALL(*stub, GetLatest(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const DataCenterProto::GetLatestRequest& req,
                          DataCenterProto::GetLatestResponse* resp) {
        EXPECT_GT(req.conn_id(), 0u);
        EXPECT_EQ(req.tags_size(), 1);
        if (req.tags_size() > 0) {
          EXPECT_EQ(req.tags(0), "reg-1");
        }
        resp->Clear();
        return grpc::Status::OK;
      }));

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  auto pair = CreatePtyPair();
  ASSERT_GE(pair.master_fd, 0);
  ASSERT_FALSE(pair.slave_path.empty());

  auto linkReq = MakeLinkReq("slave-1", pair.slave_path.c_str(), 9600, 1);
  linkReq.mutable_config()->set_mode(ModbusRTUProto::LINK_MODE_SLAVE);
  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("slave-1");
  auto* point = ptReq.add_points();
  *point = MakeRegisterPoint("reg-1", 0);
  point->set_default_uint16(0x1234);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  ASSERT_TRUE(mgr.StartLink("slave-1").ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::vector<uint8_t> reqFrame = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
  SerialBus::appendCrc(&reqFrame);
  ASSERT_TRUE(WriteAll(pair.master_fd, reqFrame));

  std::array<uint8_t, 7> resp{};
  ASSERT_TRUE(ReadExact(pair.master_fd, resp.data(), resp.size(), std::chrono::milliseconds(500)));

  EXPECT_EQ(resp[0], 0x01);
  EXPECT_EQ(resp[1], 0x03);
  EXPECT_EQ(resp[2], 0x02);
  const uint16_t value = static_cast<uint16_t>((static_cast<uint16_t>(resp[3]) << 8) | resp[4]);
  EXPECT_EQ(value, 0x1234);
  const uint16_t expectCrc = SerialBus::computeCrc(resp.data(), resp.size() - 2);
  const uint16_t gotCrc = static_cast<uint16_t>(resp[5]) | (static_cast<uint16_t>(resp[6]) << 8);
  EXPECT_EQ(gotCrc, expectCrc);

  EXPECT_TRUE(mgr.StopLink("slave-1").ok());
  CloseFd(&pair.master_fd);
}

// 验证：从站 0x03 对越界值进行截断并正常响应。
TEST(ModbusRtuLinkManagerTest, SlaveHoldingRegistersClampsOutOfRangeValue) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  EXPECT_CALL(*stub, GetLatest(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const DataCenterProto::GetLatestRequest& req,
                          DataCenterProto::GetLatestResponse* resp) {
        EXPECT_GT(req.conn_id(), 0u);
        EXPECT_EQ(req.tags_size(), 1);
        if (req.tags_size() > 0) {
          EXPECT_EQ(req.tags(0), "reg-1");
        }
        auto* update = resp->add_updates();
        update->set_dst_tag("reg-1");
        update->mutable_value()->set_int_value(70000);
        return grpc::Status::OK;
      }));

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  auto pair = CreatePtyPair();
  ASSERT_GE(pair.master_fd, 0);
  ASSERT_FALSE(pair.slave_path.empty());

  auto linkReq = MakeLinkReq("slave-2", pair.slave_path.c_str(), 9600, 1);
  linkReq.mutable_config()->set_mode(ModbusRTUProto::LINK_MODE_SLAVE);
  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("slave-2");
  *ptReq.add_points() = MakeRegisterPoint("reg-1", 0);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  ASSERT_TRUE(mgr.StartLink("slave-2").ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::vector<uint8_t> reqFrame = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
  SerialBus::appendCrc(&reqFrame);
  ASSERT_TRUE(WriteAll(pair.master_fd, reqFrame));

  std::array<uint8_t, 7> resp{};
  ASSERT_TRUE(ReadExact(pair.master_fd, resp.data(), resp.size(), std::chrono::milliseconds(500)));

  EXPECT_EQ(resp[0], 0x01);
  EXPECT_EQ(resp[1], 0x03);
  EXPECT_EQ(resp[2], 0x02);
  EXPECT_EQ(resp[3], 0xFF);
  EXPECT_EQ(resp[4], 0xFF);
  const uint16_t expectCrc = SerialBus::computeCrc(resp.data(), resp.size() - 2);
  const uint16_t gotCrc = static_cast<uint16_t>(resp[5]) | (static_cast<uint16_t>(resp[6]) << 8);
  EXPECT_EQ(gotCrc, expectCrc);

  EXPECT_TRUE(mgr.StopLink("slave-2").ok());
  CloseFd(&pair.master_fd);
}

// 验证：从站 0x03 在值缺失且无默认值时返回 0x04 异常。
TEST(ModbusRtuLinkManagerTest, SlaveHoldingRegistersReturnsExceptionOnMissingValue) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  EXPECT_CALL(*stub, GetLatest(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext*,
                          const DataCenterProto::GetLatestRequest& req,
                          DataCenterProto::GetLatestResponse* resp) {
        EXPECT_GT(req.conn_id(), 0u);
        EXPECT_EQ(req.tags_size(), 1);
        if (req.tags_size() > 0) {
          EXPECT_EQ(req.tags(0), "reg-1");
        }
        (void)resp;
        return grpc::Status::OK;
      }));

  LinkManager mgr("ModbusRTU");
  mgr.setDataCenterStub(stub);

  auto pair = CreatePtyPair();
  ASSERT_GE(pair.master_fd, 0);
  ASSERT_FALSE(pair.slave_path.empty());

  auto linkReq = MakeLinkReq("slave-2-missing", pair.slave_path.c_str(), 9600, 1);
  linkReq.mutable_config()->set_mode(ModbusRTUProto::LINK_MODE_SLAVE);
  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

  ModbusRTUProto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("slave-2-missing");
  *ptReq.add_points() = MakeRegisterPoint("reg-1", 0);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  ASSERT_TRUE(mgr.StartLink("slave-2-missing").ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::vector<uint8_t> reqFrame = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
  SerialBus::appendCrc(&reqFrame);
  ASSERT_TRUE(WriteAll(pair.master_fd, reqFrame));

  std::array<uint8_t, 5> resp{};
  ASSERT_TRUE(ReadExact(pair.master_fd, resp.data(), resp.size(), std::chrono::milliseconds(500)));

  EXPECT_EQ(resp[0], 0x01);
  EXPECT_EQ(resp[1], 0x83);
  EXPECT_EQ(resp[2], 0x04);
  const uint16_t expectCrc = SerialBus::computeCrc(resp.data(), resp.size() - 2);
  const uint16_t gotCrc = static_cast<uint16_t>(resp[3]) | (static_cast<uint16_t>(resp[4]) << 8);
  EXPECT_EQ(gotCrc, expectCrc);

  EXPECT_TRUE(mgr.StopLink("slave-2-missing").ok());
  CloseFd(&pair.master_fd);
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
