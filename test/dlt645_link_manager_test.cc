#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "DLT645LinkManager.h"
#include "DLT645PointTable.h"
#include "DataCenter_mock.grpc.pb.h"
#include "MQTTManager_mock.grpc.pb.h"
#include "support/FakeDataCenter.hpp"

namespace {
using DLT645::LinkManager;
using DLT645::PointTable;

using ::testing::_;
using ::testing::Return;

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    path_ = base / ("dlt645_link_manager_test_tmp_" + std::to_string(ts));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path &path() const {
    return path_;
  }

private:
  std::filesystem::path path_;
};

class ScopedCwd {
public:
  explicit ScopedCwd(const std::filesystem::path &newCwd) :
    old_(std::filesystem::current_path()) {
    std::filesystem::current_path(newCwd);
  }

  ~ScopedCwd() {
    std::filesystem::current_path(old_);
  }

  ScopedCwd(const ScopedCwd &) = delete;
  ScopedCwd &operator=(const ScopedCwd &) = delete;

private:
  std::filesystem::path old_;
};

struct ParsedFrame {
  std::vector<uint8_t> address;
  uint8_t control = 0;
  std::vector<uint8_t> data;
};

std::string Base64Encode(const std::vector<uint8_t> &data) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  for (size_t i = 0; i < data.size(); i += 3) {
    uint32_t a = data[i];
    uint32_t b = (i + 1 < data.size()) ? data[i + 1] : 0;
    uint32_t c = (i + 2 < data.size()) ? data[i + 2] : 0;
    uint32_t triple = (a << 16) | (b << 8) | c;
    out.push_back(kTable[(triple >> 18) & 0x3F]);
    out.push_back(kTable[(triple >> 12) & 0x3F]);
    out.push_back(i + 1 < data.size() ? kTable[(triple >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < data.size() ? kTable[triple & 0x3F] : '=');
  }
  return out;
}

}  // namespace

namespace DLT645 {
class DLT645LinkManagerTestPeer {
public:
  static grpc::Status ParseResponsePayload(LinkManager &mgr, const std::string &payloadBase64, ParsedFrame *out, std::string *error) {
    LinkManager::Frame frame;
    auto st = mgr.parseResponsePayload(payloadBase64, &frame, error);
    if (st.ok() && out != nullptr) {
      out->address = frame.address;
      out->control = frame.control;
      out->data = frame.data;
    }
    return st;
  }

  static grpc::Status DecodeAndPublish(LinkManager &mgr, const std::string &connName, const PointTable::Point &point, const std::vector<uint8_t> &payload, int64_t tsMs, bool trimRightSpace) {
    auto it = mgr.linksByName_.find(connName);
    if (it == mgr.linksByName_.end()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
    }
    return mgr.decodeAndPublish(it->second.get(), point, payload, tsMs, trimRightSpace);
  }

  static uint32_t GetConnId(LinkManager &mgr, const std::string &connName) {
    auto it = mgr.linksByName_.find(connName);
    if (it == mgr.linksByName_.end()) {
      return 0;
    }
    return it->second->connId;
  }

  static void SetLinkState(LinkManager &mgr, const std::string &connName, DLT645Proto::LinkState state) {
    auto it = mgr.linksByName_.find(connName);
    if (it == mgr.linksByName_.end()) {
      return;
    }
    it->second->state = state;
  }

  static void SetPointTableConfigured(LinkManager &mgr, const std::string &connName, bool configured) {
    auto it = mgr.linksByName_.find(connName);
    if (it == mgr.linksByName_.end()) {
      return;
    }
    it->second->pointTableConfigured = configured;
  }

  static std::vector<uint8_t> EncodeAddress(const std::string &addr) {
    return LinkManager::encodeAddress(addr);
  }

  static std::vector<uint8_t> BuildFrame(const std::vector<uint8_t> &addr, uint8_t control, const std::vector<uint8_t> &data) {
    return LinkManager::buildFrame(addr, control, data);
  }

  static void AddOffset33(std::vector<uint8_t> *data) {
    LinkManager::addOffset33(data);
  }

  static grpc::Status SendMonitorRequest(LinkManager &mgr, const std::string &connName, const std::vector<uint8_t> &frame, std::string *outPayloadBase64, int32_t *outStatus) {
    auto it = mgr.linksByName_.find(connName);
    if (it == mgr.linksByName_.end()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
    }
    return mgr.sendMonitorRequest(it->second.get(), frame, outPayloadBase64, outStatus);
  }

  static std::vector<uint8_t> EncodeData(const PointTable::Point &point, const DataCenterProto::PointValue &value, std::string *error) {
    return LinkManager::encodeData(point, value, error);
  }
};
}  // namespace DLT645
namespace {

using DLT645::DLT645LinkManagerTestPeer;

DLT645Proto::LinkConfig MakeValidLinkConfig(const char *connName, DLT645Proto::CommMode mode) {
  DLT645Proto::LinkConfig cfg;
  cfg.set_conn_name(connName);
  cfg.set_protocol_variant(DLT645Proto::PROTOCOL_VARIANT_DLT645_STD);
  cfg.set_comm_mode(mode);
  cfg.set_transport_type(DLT645Proto::TRANSPORT_UNSPECIFIED);
  cfg.set_meter_addr("123456789012");
  if (mode == DLT645Proto::COMM_MODE_SERIAL) {
    cfg.set_serial_port("RS485-1");
  }
  return cfg;
}

DLT645Proto::UpdateConfigRequest MakeMqttUpdateRequest(const char *host, uint32_t port, const char *clientId) {
  DLT645Proto::UpdateConfigRequest req;
  auto *mqtt = req.mutable_mqtt();
  mqtt->set_host(host);
  mqtt->set_port(port);
  mqtt->set_client_id(clientId);
  mqtt->set_keepalive_sec(10);
  mqtt->set_clean_session(true);
  mqtt->set_connect_timeout_ms(1000);
  return req;
}

PointTable::Point MakePoint(const char *tag, uint32_t dataLen, DLT645Proto::DataType type, double scale, double offset, double deadband) {
  PointTable::Point p;
  p.tag = tag;
  p.dataLen = dataLen;
  p.type = type;
  p.scale = scale;
  p.offset = offset;
  p.deadband = deadband;
  return p;
}

DLT645Proto::Point MakePointProto(const char *tag, const char *di, uint32_t dataLen, DLT645Proto::DataType type) {
  DLT645Proto::Point p;
  p.set_tag(tag);
  p.set_di(di);
  p.set_data_len(dataLen);
  p.set_type(type);
  p.set_access(DLT645Proto::ACCESS_READ_ONLY);
  p.set_scale(1.0);
  p.set_offset(0.0);
  p.set_deadband(0.0);
  return p;
}

DLT645Proto::BlockItem MakeBlockItemProto(const char *tag, uint32_t dataLen, DLT645Proto::DataType type) {
  DLT645Proto::BlockItem item;
  item.set_tag(tag);
  item.set_data_len(dataLen);
  item.set_type(type);
  item.set_access(DLT645Proto::ACCESS_READ_ONLY);
  item.set_scale(1.0);
  item.set_offset(0.0);
  item.set_deadband(0.0);
  return item;
}

void InstallSuccessfulSerialMqttStub(
    MQTTManagerProto::MockMQTTManagerServiceStub *mqttStub,
    std::function<void(const MQTTManagerProto::RequestAndWaitRequest &)> onRequest = {}) {
  EXPECT_CALL(*mqttStub, SubscribeRaw(_, _))
      .Times(::testing::AnyNumber())
      .WillRepeatedly([](grpc::ClientContext *, const MQTTManagerProto::SubscribeRequest &) {
        return static_cast<grpc::ClientReaderInterface<MQTTManagerProto::SubscribeResponse> *>(nullptr);
      });
  EXPECT_CALL(*mqttStub, RequestAndWait(_, _, _))
      .Times(::testing::AnyNumber())
      .WillRepeatedly(::testing::Invoke([onRequest = std::move(onRequest)](
                                            grpc::ClientContext *,
                                            const MQTTManagerProto::RequestAndWaitRequest &req,
                                            MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        if (onRequest) {
          onRequest(req);
        }
        resp->set_ok(true);
        resp->set_message("成功");
        resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
        return grpc::Status::OK;
      }));
}

bool WaitForLinkState(LinkManager &mgr,
                      const std::string &connName,
                      DLT645Proto::LinkState expectedState,
                      std::chrono::milliseconds timeout,
                      DLT645Proto::LinkInfo *out) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  DLT645Proto::LinkInfo info;
  while (std::chrono::steady_clock::now() < deadline) {
    if (mgr.GetLink(connName, &info).ok() && info.state() == expectedState) {
      if (out != nullptr) {
        *out = info;
      }
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (out != nullptr) {
    (void)mgr.GetLink(connName, out);
  }
  return false;
}
}  // namespace

// 验证：UpdateConfig 响应为空时返回参数错误。
TEST(Dlt645LinkManagerTest, UpdateConfigRejectsNullResponse) {
  LinkManager mgr("DLT645");
  DLT645Proto::UpdateConfigRequest req;
  auto st = mgr.UpdateConfig(req, nullptr);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：UpdateConfig 缺少 MQTT 配置时返回错误。
TEST(Dlt645LinkManagerTest, UpdateConfigRejectsMissingMqtt) {
  LinkManager mgr("DLT645");
  DLT645Proto::UpdateConfigRequest req;
  DLT645Proto::UpdateConfigResponse resp;
  auto st = mgr.UpdateConfig(req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(resp.ok());
}

// 验证：UpdateConfig MQTT 参数不完整时返回错误。
TEST(Dlt645LinkManagerTest, UpdateConfigRejectsIncompleteMqtt) {
  LinkManager mgr("DLT645");
  auto req = MakeMqttUpdateRequest("", 0, "");
  DLT645Proto::UpdateConfigResponse resp;
  auto st = mgr.UpdateConfig(req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(resp.ok());
}

// 验证：UpdateConfig 参数完整时返回成功。
TEST(Dlt645LinkManagerTest, UpdateConfigAcceptsValidMqtt) {
  LinkManager mgr("DLT645");
  auto req = MakeMqttUpdateRequest("127.0.0.1", 1883, "client-1");
  DLT645Proto::UpdateConfigResponse resp;
  auto st = mgr.UpdateConfig(req, &resp);
  EXPECT_TRUE(st.ok());
  EXPECT_TRUE(resp.ok());
}

// 验证：未配置 MQTT 时查询返回 configured=false，且不会把空配置当作有效配置。
TEST(Dlt645LinkManagerTest, GetConfigReportsMissingMqtt) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";
  LinkManager mgr("DLT645", configDbPath);
  DLT645Proto::GetConfigResponse resp;

  const auto status = mgr.GetConfig(&resp);
  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(resp.configured());
  EXPECT_THAT(resp.message(), ::testing::HasSubstr("MQTT 配置未配置"));
  EXPECT_FALSE(resp.has_mqtt());
}

// 验证：UpdateConfig 成功后 GetConfig 能回读当前 MQTT 配置的全部字段。
TEST(Dlt645LinkManagerTest, GetConfigReturnsCurrentMqtt) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";
  LinkManager mgr("DLT645", configDbPath);
  auto update = MakeMqttUpdateRequest("mqtt.example", 1884, "dlt645-get-config");
  update.mutable_mqtt()->set_username("user");
  update.mutable_mqtt()->set_password("secret");

  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(update, &updateResp).ok());

  DLT645Proto::GetConfigResponse resp;
  const auto status = mgr.GetConfig(&resp);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(resp.configured());
  ASSERT_TRUE(resp.has_mqtt());
  EXPECT_EQ(resp.mqtt().host(), "mqtt.example");
  EXPECT_EQ(resp.mqtt().port(), 1884u);
  EXPECT_EQ(resp.mqtt().client_id(), "dlt645-get-config");
  EXPECT_EQ(resp.mqtt().username(), "user");
  EXPECT_EQ(resp.mqtt().password(), "secret");
  EXPECT_EQ(resp.mqtt().keepalive_sec(), 10u);
  EXPECT_TRUE(resp.mqtt().clean_session());
  EXPECT_EQ(resp.mqtt().connect_timeout_ms(), 1000u);
}

// 验证：GetConfig 能从本地 SQLite 持久化配置恢复当前 MQTT 参数。
TEST(Dlt645LinkManagerTest, GetConfigReturnsPersistedMqttAfterReload) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";
  auto update = MakeMqttUpdateRequest("persisted.example", 1885, "dlt645-persisted");
  {
    LinkManager mgr("DLT645", configDbPath);
    DLT645Proto::UpdateConfigResponse updateResp;
    ASSERT_TRUE(mgr.UpdateConfig(update, &updateResp).ok());
  }

  LinkManager reloaded("DLT645", configDbPath);
  reloaded.LoadPersistedConfig();
  DLT645Proto::GetConfigResponse resp;
  ASSERT_TRUE(reloaded.GetConfig(&resp).ok());
  ASSERT_TRUE(resp.configured());
  ASSERT_TRUE(resp.has_mqtt());
  EXPECT_EQ(resp.mqtt().host(), "persisted.example");
  EXPECT_EQ(resp.mqtt().port(), 1885u);
  EXPECT_EQ(resp.mqtt().client_id(), "dlt645-persisted");
}

// 验证：UpsertLink 响应为空时返回错误。
TEST(Dlt645LinkManagerTest, UpsertLinkRejectsNullOut) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  *req.mutable_config() = MakeValidLinkConfig("conn-1", DLT645Proto::COMM_MODE_LORA);
  auto st = mgr.UpsertLink(req, nullptr);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：UpsertLink 缺少 config 时返回错误。
TEST(Dlt645LinkManagerTest, UpsertLinkRejectsMissingConfig) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  DLT645Proto::LinkInfo out;
  auto st = mgr.UpsertLink(req, &out);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：UpsertLink 校验 meter_addr 与 comm_mode。
TEST(Dlt645LinkManagerTest, UpsertLinkValidatesConfigFields) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  auto *cfg = req.mutable_config();
  cfg->set_conn_name("conn-1");
  cfg->set_protocol_variant(DLT645Proto::PROTOCOL_VARIANT_DLT645_STD);
  cfg->set_comm_mode(DLT645Proto::COMM_MODE_LORA);
  cfg->set_meter_addr("123");
  DLT645Proto::LinkInfo out;
  auto st = mgr.UpsertLink(req, &out);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：串口模式缺少 serial_port 时 UpsertLink 拒绝配置。
TEST(Dlt645LinkManagerTest, UpsertLinkSerialRejectsMissingPort) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  auto *cfg = req.mutable_config();
  cfg->set_conn_name("conn-serial-no-port");
  cfg->set_protocol_variant(DLT645Proto::PROTOCOL_VARIANT_DLT645_STD);
  cfg->set_comm_mode(DLT645Proto::COMM_MODE_SERIAL);
  cfg->set_transport_type(DLT645Proto::TRANSPORT_UNSPECIFIED);
  cfg->set_meter_addr("123456789012");

  DLT645Proto::LinkInfo out;
  auto st = mgr.UpsertLink(req, &out);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：串口模式 UpsertLink 会补齐默认串口参数。
TEST(Dlt645LinkManagerTest, UpsertLinkSerialFillsDefaultParams) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  auto *cfg = req.mutable_config();
  cfg->set_conn_name("conn-serial-default");
  cfg->set_protocol_variant(DLT645Proto::PROTOCOL_VARIANT_DLT645_STD);
  cfg->set_comm_mode(DLT645Proto::COMM_MODE_SERIAL);
  cfg->set_transport_type(DLT645Proto::TRANSPORT_UNSPECIFIED);
  cfg->set_meter_addr("123456789012");
  cfg->set_serial_port("RS485-1");

  DLT645Proto::LinkInfo out;
  ASSERT_TRUE(mgr.UpsertLink(req, &out).ok());
  EXPECT_EQ(out.config().serial_baud_rate(), 9600);
  EXPECT_EQ(out.config().serial_data_bits(), 8);
  EXPECT_EQ(out.config().serial_parity(), DLT645Proto::SERIAL_PARITY_NONE);
  EXPECT_EQ(out.config().serial_stop_bits(), DLT645Proto::SERIAL_STOP_BITS_ONE);
  EXPECT_EQ(out.config().serial_byte_timeout_ms(), 100);
  EXPECT_EQ(out.config().serial_frame_timeout_ms(), 100);
  EXPECT_EQ(out.config().serial_est_size(), 256);
  EXPECT_EQ(out.config().poll_item_interval_ms(), 0u);
}

// 验证：create_only 重复创建时返回 ALREADY_EXISTS。
TEST(Dlt645LinkManagerTest, UpsertLinkCreateOnlyRejectsDuplicate) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  *req.mutable_config() = MakeValidLinkConfig("dup", DLT645Proto::COMM_MODE_LORA);
  req.set_create_only(true);
  DLT645Proto::LinkInfo out;
  ASSERT_TRUE(mgr.UpsertLink(req, &out).ok());

  auto st = mgr.UpsertLink(req, &out);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：create_only 时若 DataCenter 已存在同名连接，则返回 ALREADY_EXISTS。
TEST(Dlt645LinkManagerTest, UpsertLinkCreateOnlyRejectsWhenDataCenterAlreadyHasKey) {
  FakeDataCenterState state;
  state.AddConnection(88, "DLT645", "dup-dc");
  auto stub = MakeStub(&state);

  EXPECT_CALL(*stub, GetOrCreateConnection(_, _, _)).Times(0);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  *req.mutable_config() = MakeValidLinkConfig("dup-dc", DLT645Proto::COMM_MODE_LORA);
  req.set_create_only(true);
  DLT645Proto::LinkInfo out;

  auto st = mgr.UpsertLink(req, &out);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：create_only=false 更新已有链路时保留 conn_id 与点表，且不会再次向 DataCenter 创建连接。
TEST(Dlt645LinkManagerTest, UpsertLinkUpdatesExistingConfigAndKeepsConnIdAndPointTable) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  EXPECT_CALL(*stub, GetOrCreateConnection(_, _, _)).Times(1);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req1;
  *req1.mutable_config() = MakeValidLinkConfig("conn-update", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo info1;
  ASSERT_TRUE(mgr.UpsertLink(req1, &info1).ok());

  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-update");
  *ptReq.add_points() = MakePointProto("P", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  DLT645Proto::UpsertLinkRequest req2;
  *req2.mutable_config() = MakeValidLinkConfig("conn-update", DLT645Proto::COMM_MODE_LORA);
  req2.mutable_config()->set_poll_interval_ms(2000);
  req2.mutable_config()->set_poll_item_interval_ms(200);
  DLT645Proto::LinkInfo info2;
  ASSERT_TRUE(mgr.UpsertLink(req2, &info2).ok());
  EXPECT_EQ(info2.conn_id(), info1.conn_id());
  EXPECT_EQ(info2.config().poll_interval_ms(), 2000u);
  EXPECT_EQ(info2.config().poll_item_interval_ms(), 200u);

  DLT645Proto::PointTable table;
  ASSERT_TRUE(mgr.GetPointTable("conn-update", &table).ok());
  ASSERT_EQ(table.points_size(), 1);
  EXPECT_EQ(table.points(0).tag(), "P");
}

// 验证：运行中或待删除链路不允许通过 UpsertLink 更新配置。
TEST(Dlt645LinkManagerTest, UpsertLinkRejectsUpdateWhenRunningOrPendingDelete) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest createReq;
  *createReq.mutable_config() = MakeValidLinkConfig("conn-state", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo created;
  ASSERT_TRUE(mgr.UpsertLink(createReq, &created).ok());

  DLT645Proto::UpsertLinkRequest updateReq;
  *updateReq.mutable_config() = MakeValidLinkConfig("conn-state", DLT645Proto::COMM_MODE_LORA);
  updateReq.mutable_config()->set_poll_interval_ms(2000);
  DLT645Proto::LinkInfo out;

  DLT645LinkManagerTestPeer::SetLinkState(mgr, "conn-state", DLT645Proto::LINK_STATE_RUNNING);
  auto st = mgr.UpsertLink(updateReq, &out);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-state", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_RUNNING);
  EXPECT_NE(got.config().poll_interval_ms(), 2000u);

  DLT645LinkManagerTestPeer::SetLinkState(mgr, "conn-state", DLT645Proto::LINK_STATE_PENDING_DELETE);
  st = mgr.UpsertLink(updateReq, &out);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  ASSERT_TRUE(mgr.GetLink("conn-state", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_PENDING_DELETE);
  EXPECT_NE(got.config().poll_interval_ms(), 2000u);
}

// 验证：RenameLink 成功后保留 conn_id，旧名字失效，新名字可继续操作点表且保留数据块。
TEST(Dlt645LinkManagerTest, RenameLinkKeepsConnIdAndMovesPointTableAndBlocks) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest createReq;
  *createReq.mutable_config() = MakeValidLinkConfig("conn-old", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo created;
  ASSERT_TRUE(mgr.UpsertLink(createReq, &created).ok());

  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-old");
  *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  auto *block = ptReq.add_blocks();
  block->set_block_di("0201FF00");
  block->set_block_data_len(2);
  *block->add_items() = MakeBlockItemProto("B1", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  DLT645Proto::LinkInfo renamed;
  ASSERT_TRUE(mgr.RenameLink("conn-old", "conn-new", &renamed).ok());
  EXPECT_EQ(renamed.conn_id(), created.conn_id());
  EXPECT_EQ(renamed.config().conn_name(), "conn-new");
  EXPECT_FALSE(state.HasConnection("DLT645", "conn-old"));
  EXPECT_TRUE(state.HasConnection("DLT645", "conn-new"));

  DLT645Proto::LinkInfo oldInfo;
  auto oldStatus = mgr.GetLink("conn-old", &oldInfo);
  EXPECT_EQ(oldStatus.error_code(), grpc::StatusCode::NOT_FOUND);

  DLT645Proto::ListLinksResponse listResp;
  ASSERT_TRUE(mgr.ListLinks(&listResp).ok());
  ASSERT_EQ(listResp.links_size(), 1);
  EXPECT_EQ(listResp.links(0).config().conn_name(), "conn-new");

  DLT645Proto::PointTable pointTable;
  ASSERT_TRUE(mgr.GetPointTable("conn-new", &pointTable).ok());
  ASSERT_EQ(pointTable.points_size(), 1);
  EXPECT_EQ(pointTable.points(0).tag(), "P1");
  ASSERT_EQ(pointTable.blocks_size(), 1);
  EXPECT_EQ(pointTable.blocks(0).block_di(), "0201FF00");
  ASSERT_EQ(pointTable.blocks(0).items_size(), 1);
  EXPECT_EQ(pointTable.blocks(0).items(0).tag(), "B1");

  DLT645Proto::UpsertPointTableRequest ptUpdateReq;
  ptUpdateReq.set_conn_name("conn-new");
  *ptUpdateReq.add_points() = MakePointProto("P2", "02010200", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptUpdateReq.set_replace(false);
  ASSERT_TRUE(mgr.UpsertPointTable(ptUpdateReq).ok());

  ASSERT_TRUE(mgr.GetPointTable("conn-new", &pointTable).ok());
  ASSERT_EQ(pointTable.points_size(), 2);
  ASSERT_EQ(pointTable.blocks_size(), 1);
  EXPECT_EQ(pointTable.blocks(0).items(0).tag(), "B1");

  ASSERT_TRUE(mgr.DeleteLink("conn-new").ok());
  EXPECT_FALSE(state.HasConnection("DLT645", "conn-new"));
}

// 验证：RenameLink 在目标名字已存在时返回 ALREADY_EXISTS。
TEST(Dlt645LinkManagerTest, RenameLinkRejectsWhenNewConnNameExists) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest oldReq;
  *oldReq.mutable_config() = MakeValidLinkConfig("conn-old", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo oldInfo;
  ASSERT_TRUE(mgr.UpsertLink(oldReq, &oldInfo).ok());

  DLT645Proto::UpsertLinkRequest newReq;
  *newReq.mutable_config() = MakeValidLinkConfig("conn-new", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo newInfo;
  ASSERT_TRUE(mgr.UpsertLink(newReq, &newInfo).ok());

  DLT645Proto::LinkInfo renamed;
  auto status = mgr.RenameLink("conn-old", "conn-new", &renamed);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：RenameLink 在旧名字不存在时返回 NOT_FOUND。
TEST(Dlt645LinkManagerTest, RenameLinkRejectsWhenOldConnNameMissing) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::LinkInfo info;
  auto status = mgr.RenameLink("missing", "conn-new", &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：运行中的连接不允许 RenameLink。
TEST(Dlt645LinkManagerTest, RenameLinkRejectsWhenLinkRunning) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest createReq;
  *createReq.mutable_config() = MakeValidLinkConfig("conn-running-rename", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo created;
  ASSERT_TRUE(mgr.UpsertLink(createReq, &created).ok());

  DLT645LinkManagerTestPeer::SetLinkState(mgr, "conn-running-rename", DLT645Proto::LINK_STATE_RUNNING);

  DLT645Proto::LinkInfo renamed;
  auto status = mgr.RenameLink("conn-running-rename", "conn-renamed", &renamed);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：UpsertLink DataCenter 创建失败会透传错误。
TEST(Dlt645LinkManagerTest, UpsertLinkPropagatesDataCenterError) {
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  ON_CALL(*stub, GetOrCreateConnection(_, _, _))
      .WillByDefault(Return(grpc::Status(grpc::StatusCode::INTERNAL, "DC 故障")));

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  *req.mutable_config() = MakeValidLinkConfig("conn-fail", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo out;
  auto st = mgr.UpsertLink(req, &out);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
}

// 验证：GetLink/ListLinks 在存在/不存在连接时返回预期。
TEST(Dlt645LinkManagerTest, GetLinkAndListLinksWork) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  *req.mutable_config() = MakeValidLinkConfig("conn-list", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-list", &got).ok());
  EXPECT_EQ(got.conn_id(), info.conn_id());

  DLT645Proto::ListLinksResponse resp;
  ASSERT_TRUE(mgr.ListLinks(&resp).ok());
  EXPECT_EQ(resp.links_size(), 1);

  auto st = mgr.GetLink("missing", &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：DeleteLink 在 DataCenter 删除失败时进入待删除状态。
TEST(Dlt645LinkManagerTest, DeleteLinkMarksPendingOnDataCenterFailure) {
  FakeDataCenterState state;
  state.FailDeleteForConnName("conn-del");
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  *req.mutable_config() = MakeValidLinkConfig("conn-del", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  auto st = mgr.DeleteLink("conn-del");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-del", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_PENDING_DELETE);
  EXPECT_FALSE(got.last_error().empty());
}

// 验证：DeleteLink 进入 PENDING_DELETE 后会落盘，重启后仍保留错误原因并阻止启动连接功能。
TEST(Dlt645LinkManagerTest, LoadPersistedConfigRestoresPendingDeleteAfterRestart) {
  ScopedTempDir dir;
  ScopedCwd cwd(dir.path());
  std::filesystem::create_directories("conf/DLT645");

  FakeDataCenterState state;
  state.FailDeleteForConnName("conn-pending-persist");
  auto stub = MakeStub(&state);

  {
    LinkManager mgr("DLT645");
    mgr.setDataCenterStub(stub);

    DLT645Proto::UpsertLinkRequest req;
    *req.mutable_config() = MakeValidLinkConfig("conn-pending-persist", DLT645Proto::COMM_MODE_LORA);
    DLT645Proto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

    auto status = mgr.DeleteLink("conn-pending-persist");
    ASSERT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  }

  {
    LinkManager mgr("DLT645");
    mgr.setDataCenterStub(stub);
    mgr.LoadPersistedConfig();

    DLT645Proto::LinkInfo info;
    ASSERT_TRUE(mgr.GetLink("conn-pending-persist", &info).ok());
    EXPECT_EQ(info.state(), DLT645Proto::LINK_STATE_PENDING_DELETE);
    EXPECT_THAT(info.last_error(), ::testing::HasSubstr("待删除"));

    auto status = mgr.StartLink("conn-pending-persist");
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  }
}

// 验证：链路配置、点表与 MQTT 配置落盘后，新实例恢复时仍会自动恢复链路连接功能。
TEST(Dlt645LinkManagerTest, LoadPersistedConfigAutoStartsRestoredReadyLink) {
  ScopedTempDir dir;
  ScopedCwd cwd(dir.path());
  std::filesystem::create_directories("conf/DLT645");

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  uint32_t connId = 0;
  {
    LinkManager mgr("DLT645");
    mgr.setDataCenterStub(stub);

    auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "persist-client");
    DLT645Proto::UpdateConfigResponse updateResp;
    ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

    DLT645Proto::UpsertLinkRequest req;
    *req.mutable_config() = MakeValidLinkConfig("conn-persist", DLT645Proto::COMM_MODE_SERIAL);
    DLT645Proto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
    connId = info.conn_id();

    DLT645Proto::UpsertPointTableRequest ptReq;
    ptReq.set_conn_name("conn-persist");
    *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
    ptReq.set_replace(true);
    ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
    ASSERT_TRUE(mgr.StopLink("conn-persist").ok());
  }

  {
    LinkManager mgr("DLT645");
    mgr.setDataCenterStub(stub);
    mgr.LoadPersistedConfig();

    DLT645Proto::LinkInfo info;
    ASSERT_TRUE(mgr.GetLink("conn-persist", &info).ok());
    EXPECT_EQ(info.conn_id(), connId);
    EXPECT_EQ(info.state(), DLT645Proto::LINK_STATE_RUNNING);
    EXPECT_TRUE(info.last_error().empty());

    DLT645Proto::PointTable pointTable;
    ASSERT_TRUE(mgr.GetPointTable("conn-persist", &pointTable).ok());
    ASSERT_EQ(pointTable.points_size(), 1);
    EXPECT_EQ(pointTable.points(0).tag(), "P1");

    ASSERT_TRUE(mgr.StopLink("conn-persist").ok());
  }
}

// 验证：持久化恢复时 DataCenter 不可用，不会把已有点表配置回写为空。
TEST(Dlt645LinkManagerTest, LoadPersistedConfigKeepsPointTablesWhenDataCenterUnavailable) {
  ScopedTempDir dir;
  ScopedCwd cwd(dir.path());
  std::filesystem::create_directories("conf/DLT645");

  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  {
    LinkManager mgr("DLT645");
    mgr.setDataCenterStub(stub);

    DLT645Proto::UpsertLinkRequest req;
    *req.mutable_config() = MakeValidLinkConfig("conn-dc-down", DLT645Proto::COMM_MODE_LORA);
    DLT645Proto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

    DLT645Proto::UpsertPointTableRequest ptReq;
    ptReq.set_conn_name("conn-dc-down");
    ptReq.set_replace(true);
    *ptReq.add_points() = MakePointProto("P-keep", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
    ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  }

  auto failingStub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  EXPECT_CALL(*failingStub, GetOrCreateConnection(_, _, _))
      .WillRepeatedly(Return(grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 未就绪")));
  {
    LinkManager mgr("DLT645");
    mgr.setDataCenterStub(failingStub);
    mgr.LoadPersistedConfig();
  }

  {
    LinkManager mgr("DLT645");
    mgr.setDataCenterStub(stub);
    mgr.LoadPersistedConfig();

    DLT645Proto::PointTable pointTable;
    ASSERT_TRUE(mgr.GetPointTable("conn-dc-down", &pointTable).ok());
    ASSERT_EQ(pointTable.points_size(), 1);
    EXPECT_EQ(pointTable.points(0).tag(), "P-keep");
  }
}

// 验证：RenameLink 落盘后，新实例恢复时仅保留新名字且数据块仍可读取。
TEST(Dlt645LinkManagerTest, LoadPersistedConfigKeepsRenamedLinkAndBlocks) {
  ScopedTempDir dir;
  ScopedCwd cwd(dir.path());
  std::filesystem::create_directories("conf/DLT645");

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  uint32_t connId = 0;
  {
    LinkManager mgr("DLT645");
    mgr.setDataCenterStub(stub);

    DLT645Proto::UpsertLinkRequest req;
    *req.mutable_config() = MakeValidLinkConfig("conn-old-persist", DLT645Proto::COMM_MODE_LORA);
    DLT645Proto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
    connId = info.conn_id();

    DLT645Proto::UpsertPointTableRequest ptReq;
    ptReq.set_conn_name("conn-old-persist");
    *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
    auto *block = ptReq.add_blocks();
    block->set_block_di("0201FF00");
    block->set_block_data_len(2);
    *block->add_items() = MakeBlockItemProto("B1", 2, DLT645Proto::DATA_TYPE_UINT16);
    ptReq.set_replace(true);
    ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
    ASSERT_TRUE(mgr.RenameLink("conn-old-persist", "conn-new-persist", &info).ok());
  }

  {
    LinkManager mgr("DLT645");
    mgr.setDataCenterStub(stub);
    mgr.LoadPersistedConfig();

    DLT645Proto::LinkInfo info;
    auto oldStatus = mgr.GetLink("conn-old-persist", &info);
    EXPECT_EQ(oldStatus.error_code(), grpc::StatusCode::NOT_FOUND);

    ASSERT_TRUE(mgr.GetLink("conn-new-persist", &info).ok());
    EXPECT_EQ(info.conn_id(), connId);

    DLT645Proto::PointTable pointTable;
    ASSERT_TRUE(mgr.GetPointTable("conn-new-persist", &pointTable).ok());
    ASSERT_EQ(pointTable.points_size(), 1);
    EXPECT_EQ(pointTable.points(0).tag(), "P1");
    ASSERT_EQ(pointTable.blocks_size(), 1);
    EXPECT_EQ(pointTable.blocks(0).block_di(), "0201FF00");
    ASSERT_EQ(pointTable.blocks(0).items_size(), 1);
    EXPECT_EQ(pointTable.blocks(0).items(0).tag(), "B1");
  }
}

// 验证：StopLink 不会把 PENDING_DELETE 状态清回 STOPPED。
TEST(Dlt645LinkManagerTest, StopLinkKeepsPendingDeleteState) {
  FakeDataCenterState state;
  state.FailDeleteForConnName("conn-pending-stop");
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  *req.mutable_config() = MakeValidLinkConfig("conn-pending-stop", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
  ASSERT_EQ(mgr.DeleteLink("conn-pending-stop").error_code(), grpc::StatusCode::INTERNAL);

  ASSERT_TRUE(mgr.StopLink("conn-pending-stop").ok());

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-pending-stop", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_PENDING_DELETE);
}

// 验证：StartLink 在未配置 MQTT 时拒绝启动连接。
TEST(Dlt645LinkManagerTest, StartLinkRejectsWithoutMqttConfig) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  *req.mutable_config() = MakeValidLinkConfig("conn-start", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
  DLT645LinkManagerTestPeer::SetPointTableConfigured(mgr, "conn-start", true);

  auto st = mgr.StartLink("conn-start");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_THAT(st.error_message(), ::testing::HasSubstr("MQTT"));
}

// 验证：StartLink 在运行中幂等成功，在待删除时拒绝启动。
TEST(Dlt645LinkManagerTest, StartLinkRejectsWhenRunningOrPendingDelete) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest req;
  *req.mutable_config() = MakeValidLinkConfig("conn-running", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  DLT645LinkManagerTestPeer::SetLinkState(mgr, "conn-running", DLT645Proto::LINK_STATE_RUNNING);
  auto st = mgr.StartLink("conn-running");
  EXPECT_TRUE(st.ok());

  DLT645LinkManagerTestPeer::SetLinkState(mgr, "conn-running", DLT645Proto::LINK_STATE_PENDING_DELETE);
  st = mgr.StartLink("conn-running");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：UpdateConfig 成功后不会自动启动已具备运行条件的停止态链路。
TEST(Dlt645LinkManagerTest, UpdateConfigKeepsStoppedWhenReadyLinkExists) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-update-config", DLT645Proto::COMM_MODE_SERIAL);
  DLT645Proto::LinkInfo linkInfo;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &linkInfo).ok());

  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-update-config");
  *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "cfg-update");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-update-config", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_STOPPED);
  EXPECT_TRUE(got.last_error().empty());
}

// 验证：UpsertPointTable 成功后链路保持 STOPPED，需显式调用 StartLink 才启动链路功能。
TEST(Dlt645LinkManagerTest, UpsertPointTableKeepsStoppedUntilExplicitStart) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();
  InstallSuccessfulSerialMqttStub(mqttStub.get());

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);
  mgr.setMqttStub(mqttStub);

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "cfg-point");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-explicit", DLT645Proto::COMM_MODE_SERIAL);
  DLT645Proto::LinkInfo linkInfo;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &linkInfo).ok());

  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-explicit");
  *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-explicit", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_STOPPED);
  EXPECT_TRUE(got.last_error().empty());

  ASSERT_TRUE(mgr.StartLink("conn-explicit").ok());
  ASSERT_TRUE(mgr.GetLink("conn-explicit", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_RUNNING);
  EXPECT_TRUE(got.last_error().empty());
  ASSERT_TRUE(mgr.StopLink("conn-explicit").ok());
}

// 验证：成功启动串口链路后即使未显式 StopLink，LinkManager 析构也会回收后台线程而不崩溃。
TEST(Dlt645LinkManagerTest, DestroyAfterStartLinkWithoutExplicitStopDoesNotCrash) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  std::mutex requestMu;
  std::condition_variable requestCv;
  size_t requestCount = 0;
  InstallSuccessfulSerialMqttStub(
      mqttStub.get(),
      [&requestMu, &requestCv, &requestCount](const MQTTManagerProto::RequestAndWaitRequest &req) {
        if (req.request_topic().find("uartManager") == std::string::npos) {
          return;
        }
        {
          std::lock_guard<std::mutex> lock(requestMu);
          ++requestCount;
        }
        requestCv.notify_all();
      });

  {
    LinkManager mgr("DLT645");
    mgr.setDataCenterStub(dcStub);
    mgr.setMqttStub(mqttStub);

    auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "cfg-destroy");
    DLT645Proto::UpdateConfigResponse updateResp;
    ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

    DLT645Proto::UpsertLinkRequest linkReq;
    *linkReq.mutable_config() = MakeValidLinkConfig("conn-destroy", DLT645Proto::COMM_MODE_SERIAL);
    DLT645Proto::LinkInfo linkInfo;
    ASSERT_TRUE(mgr.UpsertLink(linkReq, &linkInfo).ok());

    DLT645Proto::UpsertPointTableRequest ptReq;
    ptReq.set_conn_name("conn-destroy");
    *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
    ptReq.set_replace(true);
    ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

    ASSERT_TRUE(mgr.StartLink("conn-destroy").ok());
    std::unique_lock<std::mutex> lock(requestMu);
    ASSERT_TRUE(requestCv.wait_for(lock, std::chrono::seconds(3), [&requestCount]() { return requestCount >= 1; }));
  }

  EXPECT_GE(requestCount, 1u);
}

// 验证：停止态且点表已就绪的链路执行 UpsertLink 更新后，仍保持 STOPPED。
TEST(Dlt645LinkManagerTest, UpsertLinkUpdateKeepsStoppedWhenPointTableReady) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "cfg-link");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest createReq;
  *createReq.mutable_config() = MakeValidLinkConfig("conn-update-ready", DLT645Proto::COMM_MODE_SERIAL);
  DLT645Proto::LinkInfo created;
  ASSERT_TRUE(mgr.UpsertLink(createReq, &created).ok());

  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-update-ready");
  *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  ASSERT_TRUE(mgr.StopLink("conn-update-ready").ok());

  DLT645Proto::UpsertLinkRequest updateLinkReq;
  *updateLinkReq.mutable_config() = MakeValidLinkConfig("conn-update-ready", DLT645Proto::COMM_MODE_SERIAL);
  updateLinkReq.mutable_config()->set_poll_interval_ms(2000);
  DLT645Proto::LinkInfo updated;
  ASSERT_TRUE(mgr.UpsertLink(updateLinkReq, &updated).ok());
  EXPECT_EQ(updated.config().poll_interval_ms(), 2000u);

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-update-ready", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_STOPPED);
  EXPECT_TRUE(got.last_error().empty());
}

// 验证：StartLink 串口模式可启动通信任务。
TEST(Dlt645LinkManagerTest, StartLinkSerialModeStartsSuccessfully) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();
  InstallSuccessfulSerialMqttStub(mqttStub.get());

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);
  mgr.setMqttStub(mqttStub);

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c1");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest req;
  *req.mutable_config() = MakeValidLinkConfig("conn-serial", DLT645Proto::COMM_MODE_SERIAL);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
  DLT645LinkManagerTestPeer::SetPointTableConfigured(mgr, "conn-serial", true);

  auto st = mgr.StartLink("conn-serial");
  EXPECT_TRUE(st.ok());
  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-serial", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_RUNNING);
  ASSERT_TRUE(mgr.StopLink("conn-serial").ok());
}

// 验证：配置 poll_item_interval_ms 后，单次点抄收发之间会按配置等待。
TEST(Dlt645LinkManagerTest, PollingHonorsPollItemIntervalBetweenRequests) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-gap", DLT645Proto::COMM_MODE_SERIAL);
  linkReq.mutable_config()->set_poll_interval_ms(5000);
  linkReq.mutable_config()->set_poll_item_interval_ms(200);
  DLT645Proto::LinkInfo linkInfo;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &linkInfo).ok());

  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-gap");
  *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  *ptReq.add_points() = MakePointProto("P2", "02010200", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  std::mutex recordMu;
  std::condition_variable recordCv;
  std::vector<std::chrono::steady_clock::time_point> monitorTimes;
  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([&recordMu, &recordCv, &monitorTimes](
                                           grpc::ClientContext *,
                                           const MQTTManagerProto::RequestAndWaitRequest &req,
                                           MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("成功");
        if (req.request_topic().find("uartManager") != std::string::npos) {
          {
            std::lock_guard<std::mutex> lock(recordMu);
            monitorTimes.push_back(std::chrono::steady_clock::now());
          }
          recordCv.notify_all();
          resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
          return grpc::Status::OK;
        }
        resp->set_payload("{\"status\":0}");
        return grpc::Status::OK;
      }));

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-gap");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-gap", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_STOPPED);
  ASSERT_TRUE(mgr.StartLink("conn-gap").ok());

  {
    std::unique_lock<std::mutex> lock(recordMu);
    ASSERT_TRUE(recordCv.wait_for(lock, std::chrono::seconds(3), [&monitorTimes]() { return monitorTimes.size() >= 2; }));
  }

  ASSERT_TRUE(mgr.StopLink("conn-gap").ok());

  std::vector<std::chrono::steady_clock::time_point> capturedTimes;
  {
    std::lock_guard<std::mutex> lock(recordMu);
    capturedTimes = monitorTimes;
  }
  ASSERT_GE(capturedTimes.size(), 2u);
  const auto intervalMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(capturedTimes[1] - capturedTimes[0]).count();
  EXPECT_GE(intervalMs, 150);
}

// 验证：LoRa 轮询点抄收到非零 status 后，会按 request_timeout_ms 等待再抄下一个点。
TEST(Dlt645LinkManagerTest, LoraPollingWaitsRequestTimeoutAfterNonZeroStatus) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-lora-gap", DLT645Proto::COMM_MODE_LORA);
  linkReq.mutable_config()->set_poll_interval_ms(60000);
  linkReq.mutable_config()->set_poll_item_interval_ms(0);
  linkReq.mutable_config()->set_request_timeout_ms(1500);
  DLT645Proto::LinkInfo linkInfo;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &linkInfo).ok());

  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-lora-gap");
  *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  *ptReq.add_points() = MakePointProto("P2", "02010200", 2, DLT645Proto::DATA_TYPE_UINT16);
  *ptReq.add_points() = MakePointProto("P3", "02010300", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  std::mutex recordMu;
  std::condition_variable recordCv;
  std::vector<std::chrono::steady_clock::time_point> monitorTimes;
  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([&recordMu, &recordCv, &monitorTimes](
                                           grpc::ClientContext *,
                                           const MQTTManagerProto::RequestAndWaitRequest &req,
                                           MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("成功");
        if (req.request_topic().find("addslaveNode") != std::string::npos) {
          resp->set_payload("{\"status\":2}");
          return grpc::Status::OK;
        }
        if (req.request_topic().find("delslaveNode") != std::string::npos) {
          resp->set_payload("{\"status\":0}");
          return grpc::Status::OK;
        }
        if (req.request_topic().find("loraManager") != std::string::npos &&
            req.request_topic().find("monitorNode") != std::string::npos) {
          size_t currentCount = 0;
          {
            std::lock_guard<std::mutex> lock(recordMu);
            monitorTimes.push_back(std::chrono::steady_clock::now());
            currentCount = monitorTimes.size();
          }
          recordCv.notify_all();
          if (currentCount == 1 || currentCount == 3) {
            resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
          } else if (currentCount == 2) {
            resp->set_payload("{\"status\":9}");
          } else {
            resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
          }
          return grpc::Status::OK;
        }
        resp->set_payload("{\"status\":0}");
        return grpc::Status::OK;
      }));

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-lora-gap");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-lora-gap", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_STOPPED);
  ASSERT_TRUE(mgr.StartLink("conn-lora-gap").ok());
  ASSERT_TRUE(WaitForLinkState(
      mgr, "conn-lora-gap", DLT645Proto::LINK_STATE_RUNNING, std::chrono::seconds(3), &got));

  {
    std::unique_lock<std::mutex> lock(recordMu);
    ASSERT_TRUE(recordCv.wait_for(lock, std::chrono::seconds(8), [&monitorTimes]() { return monitorTimes.size() >= 3; }));
  }

  ASSERT_TRUE(mgr.StopLink("conn-lora-gap").ok());

  std::vector<std::chrono::steady_clock::time_point> capturedTimes;
  {
    std::lock_guard<std::mutex> lock(recordMu);
    capturedTimes = monitorTimes;
  }
  ASSERT_GE(capturedTimes.size(), 3u);
  const auto firstGapMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(capturedTimes[1] - capturedTimes[0]).count();
  const auto secondGapMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(capturedTimes[2] - capturedTimes[1]).count();
  EXPECT_LT(firstGapMs, 1000);
  EXPECT_GE(secondGapMs, 1300);
  EXPECT_LT(secondGapMs, 3000);
}

// 验证：LoRa 轮询数据块收到非零 status 后，会按 request_timeout_ms 等待再抄下一个数据块。
TEST(Dlt645LinkManagerTest, LoraBlockPollingWaitsRequestTimeoutAfterNonZeroStatus) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-lora-block-gap", DLT645Proto::COMM_MODE_LORA);
  linkReq.mutable_config()->set_poll_interval_ms(60000);
  linkReq.mutable_config()->set_poll_item_interval_ms(0);
  linkReq.mutable_config()->set_request_timeout_ms(1500);
  DLT645Proto::LinkInfo linkInfo;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &linkInfo).ok());

  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-lora-block-gap");
  auto *block1 = ptReq.add_blocks();
  block1->set_block_di("0201FF00");
  block1->set_block_data_len(2);
  *block1->add_items() = MakeBlockItemProto("B1", 2, DLT645Proto::DATA_TYPE_UINT16);
  auto *block2 = ptReq.add_blocks();
  block2->set_block_di("0202FF00");
  block2->set_block_data_len(2);
  *block2->add_items() = MakeBlockItemProto("B2", 2, DLT645Proto::DATA_TYPE_UINT16);
  auto *block3 = ptReq.add_blocks();
  block3->set_block_di("0203FF00");
  block3->set_block_data_len(2);
  *block3->add_items() = MakeBlockItemProto("B3", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  std::mutex recordMu;
  std::condition_variable recordCv;
  std::vector<std::chrono::steady_clock::time_point> monitorTimes;
  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([&recordMu, &recordCv, &monitorTimes](
                                           grpc::ClientContext *,
                                           const MQTTManagerProto::RequestAndWaitRequest &req,
                                           MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("成功");
        if (req.request_topic().find("addslaveNode") != std::string::npos) {
          resp->set_payload("{\"status\":2}");
          return grpc::Status::OK;
        }
        if (req.request_topic().find("delslaveNode") != std::string::npos) {
          resp->set_payload("{\"status\":0}");
          return grpc::Status::OK;
        }
        if (req.request_topic().find("loraManager") != std::string::npos &&
            req.request_topic().find("monitorNode") != std::string::npos) {
          size_t currentCount = 0;
          {
            std::lock_guard<std::mutex> lock(recordMu);
            monitorTimes.push_back(std::chrono::steady_clock::now());
            currentCount = monitorTimes.size();
          }
          recordCv.notify_all();
          if (currentCount == 2) {
            resp->set_payload("{\"status\":9}");
          } else {
            resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
          }
          return grpc::Status::OK;
        }
        resp->set_payload("{\"status\":0}");
        return grpc::Status::OK;
      }));

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-lora-block-gap");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-lora-block-gap", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_STOPPED);
  ASSERT_TRUE(mgr.StartLink("conn-lora-block-gap").ok());
  ASSERT_TRUE(WaitForLinkState(
      mgr, "conn-lora-block-gap", DLT645Proto::LINK_STATE_RUNNING, std::chrono::seconds(3), &got));

  {
    std::unique_lock<std::mutex> lock(recordMu);
    ASSERT_TRUE(recordCv.wait_for(lock, std::chrono::seconds(6), [&monitorTimes]() { return monitorTimes.size() >= 3; }));
  }

  ASSERT_TRUE(mgr.StopLink("conn-lora-block-gap").ok());

  std::vector<std::chrono::steady_clock::time_point> capturedTimes;
  {
    std::lock_guard<std::mutex> lock(recordMu);
    capturedTimes = monitorTimes;
  }
  ASSERT_GE(capturedTimes.size(), 3u);
  const auto firstGapMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(capturedTimes[1] - capturedTimes[0]).count();
  const auto secondGapMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(capturedTimes[2] - capturedTimes[1]).count();
  EXPECT_LT(firstGapMs, 1000);
  EXPECT_GE(secondGapMs, 1300);
  EXPECT_LT(secondGapMs, 3000);
}

// 验证：Lora 链路启动连接功能阻塞时，不会阻塞 UART 链路启动连接功能。
TEST(Dlt645LinkManagerTest, StartLinkLoraBlockedDoesNotBlockUart) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c2");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest loraReq;
  *loraReq.mutable_config() = MakeValidLinkConfig("conn-lora", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo loraInfo;
  ASSERT_TRUE(mgr.UpsertLink(loraReq, &loraInfo).ok());
  DLT645LinkManagerTestPeer::SetPointTableConfigured(mgr, "conn-lora", true);

  DLT645Proto::UpsertLinkRequest uartReq;
  *uartReq.mutable_config() = MakeValidLinkConfig("conn-uart", DLT645Proto::COMM_MODE_SERIAL);
  DLT645Proto::LinkInfo uartInfo;
  ASSERT_TRUE(mgr.UpsertLink(uartReq, &uartInfo).ok());
  DLT645LinkManagerTestPeer::SetPointTableConfigured(mgr, "conn-uart", true);

  std::mutex gateMu;
  std::condition_variable gateCv;
  bool loraEntered = false;
  bool releaseLora = false;
  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([&gateMu, &gateCv, &loraEntered, &releaseLora](
                                           grpc::ClientContext *,
                                           const MQTTManagerProto::RequestAndWaitRequest &req,
                                           MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("OK");
        if (req.request_topic().find("addslaveNode") != std::string::npos) {
          {
            std::lock_guard<std::mutex> lock(gateMu);
            loraEntered = true;
          }
          gateCv.notify_all();
          std::unique_lock<std::mutex> lock(gateMu);
          gateCv.wait_for(lock, std::chrono::seconds(3), [&releaseLora]() { return releaseLora; });
          resp->set_payload("{\"status\":0}");
          return grpc::Status::OK;
        }
        if (req.request_topic().find("delslaveNode") != std::string::npos) {
          resp->set_payload("{\"status\":0}");
          return grpc::Status::OK;
        }
        resp->set_payload("{\"status\":1}");
        return grpc::Status::OK;
      }));

  auto loraFuture = std::async(std::launch::async, [&mgr]() { return mgr.StartLink("conn-lora"); });
  {
    std::unique_lock<std::mutex> lock(gateMu);
    ASSERT_TRUE(gateCv.wait_for(lock, std::chrono::seconds(2), [&loraEntered]() { return loraEntered; }));
  }

  const auto uartStartBegin = std::chrono::steady_clock::now();
  auto uartStatus = mgr.StartLink("conn-uart");
  const auto uartCostMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - uartStartBegin).count();
  EXPECT_TRUE(uartStatus.ok());
  EXPECT_LT(uartCostMs, 1000);

  {
    std::lock_guard<std::mutex> lock(gateMu);
    releaseLora = true;
  }
  gateCv.notify_all();
  auto loraStatus = loraFuture.get();
  EXPECT_TRUE(loraStatus.ok());

  DLT645Proto::LinkInfo loraGot;
  DLT645Proto::LinkInfo uartGot;
  ASSERT_TRUE(mgr.GetLink("conn-lora", &loraGot).ok());
  ASSERT_TRUE(mgr.GetLink("conn-uart", &uartGot).ok());
  EXPECT_EQ(loraGot.state(), DLT645Proto::LINK_STATE_RUNNING);
  EXPECT_EQ(uartGot.state(), DLT645Proto::LINK_STATE_RUNNING);
  ASSERT_TRUE(mgr.StopLink("conn-uart").ok());
  ASSERT_TRUE(mgr.StopLink("conn-lora").ok());
}

// 验证：多个 Lora 连接的点抄请求在模块内按全局串行顺序执行。
TEST(Dlt645LinkManagerTest, LoraMonitorRequestsSerializeAcrossConnections) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-lora-serial");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest reqA;
  *reqA.mutable_config() = MakeValidLinkConfig("conn-lora-a", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo infoA;
  ASSERT_TRUE(mgr.UpsertLink(reqA, &infoA).ok());

  DLT645Proto::UpsertLinkRequest reqB;
  *reqB.mutable_config() = MakeValidLinkConfig("conn-lora-b", DLT645Proto::COMM_MODE_LORA);
  reqB.mutable_config()->set_meter_addr("123456789013");
  DLT645Proto::LinkInfo infoB;
  ASSERT_TRUE(mgr.UpsertLink(reqB, &infoB).ok());

  std::mutex gateMu;
  std::condition_variable gateCv;
  int activeLora = 0;
  int maxActiveLora = 0;
  int loraCalls = 0;
  bool firstEntered = false;
  bool secondEntered = false;
  bool releaseFirst = false;
  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([&gateMu,
                                        &gateCv,
                                        &activeLora,
                                        &maxActiveLora,
                                        &loraCalls,
                                        &firstEntered,
                                        &secondEntered,
                                        &releaseFirst](
                                           grpc::ClientContext *,
                                           const MQTTManagerProto::RequestAndWaitRequest &req,
                                           MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("成功");
        if (req.request_topic().find("loraManager") != std::string::npos &&
            req.request_topic().find("monitorNode") != std::string::npos) {
          std::unique_lock<std::mutex> lock(gateMu);
          ++activeLora;
          if (activeLora > maxActiveLora) {
            maxActiveLora = activeLora;
          }
          ++loraCalls;
          if (loraCalls == 1) {
            firstEntered = true;
            gateCv.notify_all();
            gateCv.wait_for(lock, std::chrono::seconds(3), [&releaseFirst]() { return releaseFirst; });
          } else if (loraCalls == 2) {
            secondEntered = true;
            gateCv.notify_all();
          }
          --activeLora;
        }
        resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
        return grpc::Status::OK;
      }));

  const std::vector<uint8_t> frame = {0x68, 0x11, 0x22};
  auto sendMonitor = [&mgr, &frame](const std::string &connName) {
    std::string payloadBase64;
    int32_t status = -1;
    auto st = DLT645LinkManagerTestPeer::SendMonitorRequest(mgr, connName, frame, &payloadBase64, &status);
    if (!st.ok()) {
      return st;
    }
    if (status != 0) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "状态码非零");
    }
    if (payloadBase64.empty()) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "响应数据为空");
    }
    return grpc::Status::OK;
  };

  auto futureA = std::async(std::launch::async, [&sendMonitor]() { return sendMonitor("conn-lora-a"); });
  {
    std::unique_lock<std::mutex> lock(gateMu);
    ASSERT_TRUE(gateCv.wait_for(lock, std::chrono::seconds(2), [&firstEntered]() { return firstEntered; }));
  }

  auto futureB = std::async(std::launch::async, [&sendMonitor]() { return sendMonitor("conn-lora-b"); });
  {
    std::unique_lock<std::mutex> lock(gateMu);
    EXPECT_FALSE(gateCv.wait_for(lock, std::chrono::milliseconds(300), [&secondEntered]() { return secondEntered; }));
  }

  {
    std::lock_guard<std::mutex> lock(gateMu);
    releaseFirst = true;
  }
  gateCv.notify_all();

  auto stA = futureA.get();
  auto stB = futureB.get();
  EXPECT_TRUE(stA.ok());
  EXPECT_TRUE(stB.ok());
  EXPECT_EQ(loraCalls, 2);
  EXPECT_TRUE(secondEntered);
  EXPECT_EQ(maxActiveLora, 1);
}

// 验证：Lora 点抄请求阻塞时，不会阻塞 Carrier 点抄请求。
TEST(Dlt645LinkManagerTest, LoraMonitorBlockedDoesNotBlockCarrier) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-lora-carrier");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest loraReq;
  *loraReq.mutable_config() = MakeValidLinkConfig("conn-lora-monitor", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo loraInfo;
  ASSERT_TRUE(mgr.UpsertLink(loraReq, &loraInfo).ok());

  DLT645Proto::UpsertLinkRequest carrierReq;
  *carrierReq.mutable_config() = MakeValidLinkConfig("conn-carrier-monitor", DLT645Proto::COMM_MODE_CARRIER);
  carrierReq.mutable_config()->set_meter_addr("123456789014");
  DLT645Proto::LinkInfo carrierInfo;
  ASSERT_TRUE(mgr.UpsertLink(carrierReq, &carrierInfo).ok());

  std::mutex gateMu;
  std::condition_variable gateCv;
  bool loraEntered = false;
  bool releaseLora = false;
  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([&gateMu, &gateCv, &loraEntered, &releaseLora](
                                           grpc::ClientContext *,
                                           const MQTTManagerProto::RequestAndWaitRequest &req,
                                           MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("成功");
        if (req.request_topic().find("loraManager") != std::string::npos &&
            req.request_topic().find("monitorNode") != std::string::npos) {
          {
            std::lock_guard<std::mutex> lock(gateMu);
            loraEntered = true;
          }
          gateCv.notify_all();
          std::unique_lock<std::mutex> lock(gateMu);
          gateCv.wait_for(lock, std::chrono::seconds(3), [&releaseLora]() { return releaseLora; });
        }
        resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
        return grpc::Status::OK;
      }));

  const std::vector<uint8_t> frame = {0x68, 0x33, 0x44};
  auto loraFuture = std::async(std::launch::async, [&mgr, &frame]() {
    std::string payloadBase64;
    int32_t status = -1;
    return DLT645LinkManagerTestPeer::SendMonitorRequest(mgr, "conn-lora-monitor", frame, &payloadBase64, &status);
  });
  {
    std::unique_lock<std::mutex> lock(gateMu);
    ASSERT_TRUE(gateCv.wait_for(lock, std::chrono::seconds(2), [&loraEntered]() { return loraEntered; }));
  }

  const auto carrierStartBegin = std::chrono::steady_clock::now();
  std::string carrierPayloadBase64;
  int32_t carrierStatus = -1;
  auto carrierSt = DLT645LinkManagerTestPeer::SendMonitorRequest(
      mgr, "conn-carrier-monitor", frame, &carrierPayloadBase64, &carrierStatus);
  const auto carrierCostMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - carrierStartBegin)
          .count();
  EXPECT_TRUE(carrierSt.ok());
  EXPECT_EQ(carrierStatus, 0);
  EXPECT_FALSE(carrierPayloadBase64.empty());
  EXPECT_LT(carrierCostMs, 1000);

  {
    std::lock_guard<std::mutex> lock(gateMu);
    releaseLora = true;
  }
  gateCv.notify_all();

  auto loraSt = loraFuture.get();
  EXPECT_TRUE(loraSt.ok());
}

// 验证：同一 Lora 地址的多个连接仅在首启/末停时各执行一次档案增删。
TEST(Dlt645LinkManagerTest, StartStopLoraSameAddrSharesArchiveLifecycle) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-share");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest reqA;
  *reqA.mutable_config() = MakeValidLinkConfig("conn-share-a", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo infoA;
  ASSERT_TRUE(mgr.UpsertLink(reqA, &infoA).ok());
  DLT645LinkManagerTestPeer::SetPointTableConfigured(mgr, "conn-share-a", true);

  DLT645Proto::UpsertLinkRequest reqB;
  *reqB.mutable_config() = MakeValidLinkConfig("conn-share-b", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo infoB;
  ASSERT_TRUE(mgr.UpsertLink(reqB, &infoB).ok());
  DLT645LinkManagerTestPeer::SetPointTableConfigured(mgr, "conn-share-b", true);

  std::atomic<int> addCount{0};
  std::atomic<int> delCount{0};
  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([&addCount, &delCount](
                                           grpc::ClientContext *,
                                           const MQTTManagerProto::RequestAndWaitRequest &req,
                                           MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("成功");
        if (req.request_topic().find("addslaveNode") != std::string::npos) {
          addCount.fetch_add(1);
        }
        if (req.request_topic().find("delslaveNode") != std::string::npos) {
          delCount.fetch_add(1);
        }
        resp->set_payload("{\"status\":0}");
        return grpc::Status::OK;
      }));

  ASSERT_TRUE(mgr.StartLink("conn-share-a").ok());
  ASSERT_TRUE(mgr.StartLink("conn-share-b").ok());
  EXPECT_EQ(addCount.load(), 1);

  ASSERT_TRUE(mgr.StopLink("conn-share-a").ok());
  EXPECT_EQ(delCount.load(), 0);

  ASSERT_TRUE(mgr.StopLink("conn-share-b").ok());
  EXPECT_EQ(delCount.load(), 1);
}

// 验证：同一批次多个 LoRa 档案添加会合并为单条 addslaveNode MQTT 消息，body 数组包含全部档案项。
TEST(Dlt645LinkManagerTest, TryAutoStartReadyLinksBatchesLoraArchivesIntoSingleMqttMessage) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  DLT645Proto::UpsertLinkRequest reqA;
  *reqA.mutable_config() = MakeValidLinkConfig("conn-batch-a", DLT645Proto::COMM_MODE_LORA);
  reqA.mutable_config()->set_meter_addr("123456789012");
  DLT645Proto::LinkInfo infoA;
  ASSERT_TRUE(mgr.UpsertLink(reqA, &infoA).ok());

  DLT645Proto::UpsertLinkRequest reqB;
  *reqB.mutable_config() = MakeValidLinkConfig("conn-batch-b", DLT645Proto::COMM_MODE_LORA);
  reqB.mutable_config()->set_meter_addr("123456789013");
  DLT645Proto::LinkInfo infoB;
  ASSERT_TRUE(mgr.UpsertLink(reqB, &infoB).ok());

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-batch-add");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645LinkManagerTestPeer::SetPointTableConfigured(mgr, "conn-batch-a", true);
  DLT645LinkManagerTestPeer::SetPointTableConfigured(mgr, "conn-batch-b", true);

  std::mutex addMu;
  std::vector<std::string> addPayloads;
  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([&addMu, &addPayloads](
                                           grpc::ClientContext *,
                                           const MQTTManagerProto::RequestAndWaitRequest &req,
                                           MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("成功");
        if (req.request_topic().find("addslaveNode") != std::string::npos) {
          std::lock_guard<std::mutex> lock(addMu);
          addPayloads.push_back(req.payload());
        }
        resp->set_payload("{\"status\":0}");
        return grpc::Status::OK;
      }));

  mgr.TryAutoStartReadyLinks("批量档案下发测试");

  EXPECT_TRUE(mgr.StopLink("conn-batch-a").ok());
  EXPECT_TRUE(mgr.StopLink("conn-batch-b").ok());

  EXPECT_EQ(addPayloads.size(), 1u);
  if (addPayloads.size() != 1u) {
    return;
  }

  boost::system::error_code ec;
  auto parsed = boost::json::parse(addPayloads.front(), ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(parsed.is_object());

  const auto &obj = parsed.as_object();
  auto bodyIt = obj.find("body");
  ASSERT_NE(bodyIt, obj.end());
  ASSERT_TRUE(bodyIt->value().is_array());

  const auto &body = bodyIt->value().as_array();
  EXPECT_EQ(body.size(), 2u);

  std::set<std::string> addrs;
  for (const auto &item : body) {
    ASSERT_TRUE(item.is_object());
    const auto &itemObj = item.as_object();

    auto addrIt = itemObj.find("addr");
    ASSERT_NE(addrIt, itemObj.end());
    ASSERT_TRUE(addrIt->value().is_string());
    addrs.insert(std::string(addrIt->value().as_string().c_str()));

    auto proTypeIt = itemObj.find("proType");
    ASSERT_NE(proTypeIt, itemObj.end());
    ASSERT_TRUE(proTypeIt->value().is_int64() || proTypeIt->value().is_uint64());
    const int64_t proType = proTypeIt->value().is_int64()
        ? proTypeIt->value().as_int64()
        : static_cast<int64_t>(proTypeIt->value().as_uint64());
    EXPECT_EQ(proType, 2);
  }

  EXPECT_EQ(addrs, (std::set<std::string>{"123456789012", "123456789013"}));
}

// 验证：显式 StartLink 时 addslaveNode 返回数值状态 2，会视为档案已存在并继续抄表，且 StopLink 仍会删除档案。
TEST(Dlt645LinkManagerTest, StartLinkTreatsNumericArchiveStatus2AsExistingAndDeletesOnStop) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  std::mutex requestMu;
  std::condition_variable requestCv;
  int addCount = 0;
  int delCount = 0;
  int monitorCount = 0;
  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([&requestMu, &requestCv, &addCount, &delCount, &monitorCount](
                                           grpc::ClientContext *,
                                           const MQTTManagerProto::RequestAndWaitRequest &req,
                                           MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("成功");
        {
          std::lock_guard<std::mutex> lock(requestMu);
          if (req.request_topic().find("addslaveNode") != std::string::npos) {
            ++addCount;
            resp->set_payload("{\"status\":2}");
          } else if (req.request_topic().find("delslaveNode") != std::string::npos) {
            ++delCount;
            resp->set_payload("{\"status\":0}");
          } else if (req.request_topic().find("loraManager") != std::string::npos &&
                     req.request_topic().find("monitorNode") != std::string::npos) {
            ++monitorCount;
            resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
          } else {
            resp->set_payload("{\"status\":0}");
          }
        }
        requestCv.notify_all();
        return grpc::Status::OK;
      }));

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-archive-exists");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-archive-exists", DLT645Proto::COMM_MODE_LORA);
  linkReq.mutable_config()->set_poll_interval_ms(200);
  DLT645Proto::LinkInfo linkInfo;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &linkInfo).ok());

  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-archive-exists");
  *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  ASSERT_TRUE(mgr.StartLink("conn-archive-exists").ok());

  {
    std::unique_lock<std::mutex> lock(requestMu);
    ASSERT_TRUE(requestCv.wait_for(lock, std::chrono::seconds(2), [&addCount]() { return addCount >= 1; }));
  }

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(WaitForLinkState(
      mgr, "conn-archive-exists", DLT645Proto::LINK_STATE_RUNNING, std::chrono::seconds(2), &got));
  EXPECT_TRUE(got.last_error().empty());

  {
    std::unique_lock<std::mutex> lock(requestMu);
    ASSERT_TRUE(requestCv.wait_for(lock, std::chrono::seconds(3), [&monitorCount]() { return monitorCount >= 1; }));
    EXPECT_FALSE(requestCv.wait_for(lock, std::chrono::seconds(6), [&addCount]() { return addCount >= 2; }));
    EXPECT_EQ(addCount, 1);
  }

  ASSERT_TRUE(mgr.StopLink("conn-archive-exists").ok());
  {
    std::lock_guard<std::mutex> lock(requestMu);
    EXPECT_EQ(delCount, 1);
  }

  ASSERT_TRUE(mgr.GetLink("conn-archive-exists", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_STOPPED);
}

// 验证：显式 StartLink 失败时，会把 LoRa status 转成中文原因写入 last_error。
TEST(Dlt645LinkManagerTest, StartLinkStoresChineseArchiveFailureReasonInLastError) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([](grpc::ClientContext *,
                                          const MQTTManagerProto::RequestAndWaitRequest &req,
                                          MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("成功");
        if (req.request_topic().find("addslaveNode") != std::string::npos) {
          resp->set_payload("{\"status\":\"Buffull\"}");
        } else if (req.request_topic().find("delslaveNode") != std::string::npos) {
          resp->set_payload("{\"status\":0}");
        } else {
          resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
        }
        return grpc::Status::OK;
      }));

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-archive-reason");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-archive-reason", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo linkInfo;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &linkInfo).ok());

  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-archive-reason");
  *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  auto st = mgr.StartLink("conn-archive-reason");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-archive-reason", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_STOPPED);
  EXPECT_EQ(got.last_error(), "档案添加失败: 缓冲区满");

  ASSERT_TRUE(mgr.StopLink("conn-archive-reason").ok());
}

// 验证：显式 StartLink 遇到档案响应缺少 status 时，会直接按失败处理并记录中文原因。
TEST(Dlt645LinkManagerTest, StartLinkRejectsArchiveResponseMissingStatus) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([](grpc::ClientContext *,
                                          const MQTTManagerProto::RequestAndWaitRequest &req,
                                          MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("成功");
        if (req.request_topic().find("addslaveNode") != std::string::npos) {
          resp->set_payload("{\"token\":\"x\"}");
        } else if (req.request_topic().find("delslaveNode") != std::string::npos) {
          resp->set_payload("{\"status\":0}");
        } else {
          resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
        }
        return grpc::Status::OK;
      }));

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-archive-missing");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-archive-missing", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo linkInfo;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &linkInfo).ok());

  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-archive-missing");
  *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  auto st = mgr.StartLink("conn-archive-missing");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-archive-missing", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_STOPPED);
  EXPECT_EQ(got.last_error(), "档案添加响应缺少状态字段");

  ASSERT_TRUE(mgr.StopLink("conn-archive-missing").ok());
}

// 验证：显式 StartLink 首轮档案添加失败后，会在后台按 5 秒间隔继续重试直到成功。
TEST(Dlt645LinkManagerTest, StartLinkRetriesArchiveAddUntilSuccess) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  std::mutex addMu;
  std::vector<std::chrono::steady_clock::time_point> addTimes;
  std::atomic<int> addCount{0};
  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([&addMu, &addTimes, &addCount](
                                           grpc::ClientContext *,
                                           const MQTTManagerProto::RequestAndWaitRequest &req,
                                           MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("成功");
        if (req.request_topic().find("addslaveNode") != std::string::npos) {
          {
            std::lock_guard<std::mutex> lock(addMu);
            addTimes.push_back(std::chrono::steady_clock::now());
          }
          const int current = addCount.fetch_add(1) + 1;
          if (current == 1) {
            resp->set_payload("{\"status\":\"Frametimeout\"}");
          } else {
            resp->set_payload("{\"status\":0}");
          }
        } else if (req.request_topic().find("delslaveNode") != std::string::npos) {
          resp->set_payload("{\"status\":0}");
        } else {
          resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
        }
        return grpc::Status::OK;
      }));

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-archive-retry");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-archive-retry", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo linkInfo;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &linkInfo).ok());

  const auto firstAttemptBegin = std::chrono::steady_clock::now();
  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-archive-retry");
  *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  auto st = mgr.StartLink("conn-archive-retry");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-archive-retry", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_STOPPED);
  EXPECT_EQ(got.last_error(), "档案添加失败: 帧超时");

  ASSERT_TRUE(WaitForLinkState(
      mgr, "conn-archive-retry", DLT645Proto::LINK_STATE_RUNNING, std::chrono::seconds(8), &got));
  EXPECT_TRUE(got.last_error().empty());
  EXPECT_GE(addCount.load(), 2);

  std::vector<std::chrono::steady_clock::time_point> capturedTimes;
  {
    std::lock_guard<std::mutex> lock(addMu);
    capturedTimes = addTimes;
  }
  ASSERT_GE(capturedTimes.size(), 2u);
  const auto retryGapMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(capturedTimes[1] - capturedTimes[0]).count();
  EXPECT_GE(retryGapMs, 4500);
  const auto totalCostMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(capturedTimes[1] - firstAttemptBegin).count();
  EXPECT_GE(totalCostMs, 4500);

  ASSERT_TRUE(mgr.StopLink("conn-archive-retry").ok());
}

// 验证：显式 StartLink 首次等待 MQTT 响应超时后，会在后台按 5 秒间隔重新发送 addslaveNode 直到成功。
TEST(Dlt645LinkManagerTest, StartLinkRetriesArchiveAddAfterMqttTimeout) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  std::mutex addMu;
  std::vector<std::chrono::steady_clock::time_point> addTimes;
  std::atomic<int> addCount{0};
  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([&addMu, &addTimes, &addCount](
                                           grpc::ClientContext *,
                                           const MQTTManagerProto::RequestAndWaitRequest &req,
                                           MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        if (req.request_topic().find("addslaveNode") != std::string::npos) {
          {
            std::lock_guard<std::mutex> lock(addMu);
            addTimes.push_back(std::chrono::steady_clock::now());
          }
          const int current = addCount.fetch_add(1) + 1;
          if (current == 1) {
            return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "等待响应超时");
          }
          resp->set_ok(true);
          resp->set_message("成功");
          resp->set_payload("{\"status\":0}");
          return grpc::Status::OK;
        }

        resp->set_ok(true);
        resp->set_message("成功");
        if (req.request_topic().find("delslaveNode") != std::string::npos) {
          resp->set_payload("{\"status\":0}");
        } else {
          resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
        }
        return grpc::Status::OK;
      }));

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-archive-timeout");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-archive-timeout", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo linkInfo;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &linkInfo).ok());

  const auto firstAttemptBegin = std::chrono::steady_clock::now();
  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-archive-timeout");
  *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  auto st = mgr.StartLink("conn-archive-timeout");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-archive-timeout", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_STOPPED);
  EXPECT_EQ(got.last_error(), "等待响应超时");

  ASSERT_TRUE(WaitForLinkState(
      mgr, "conn-archive-timeout", DLT645Proto::LINK_STATE_RUNNING, std::chrono::seconds(8), &got));
  EXPECT_TRUE(got.last_error().empty());
  EXPECT_GE(addCount.load(), 2);

  std::vector<std::chrono::steady_clock::time_point> capturedTimes;
  {
    std::lock_guard<std::mutex> lock(addMu);
    capturedTimes = addTimes;
  }
  ASSERT_GE(capturedTimes.size(), 2u);
  const auto retryGapMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(capturedTimes[1] - capturedTimes[0]).count();
  EXPECT_GE(retryGapMs, 4500);
  const auto totalCostMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(capturedTimes[1] - firstAttemptBegin).count();
  EXPECT_GE(totalCostMs, 4500);

  ASSERT_TRUE(mgr.StopLink("conn-archive-timeout").ok());
}

// 验证：StopLink 会终止显式 StartLink 失败后触发的档案后台重试，不会在 5 秒后继续重发 addslaveNode。
TEST(Dlt645LinkManagerTest, StopLinkCancelsArchiveRetry) {
  FakeDataCenterState state;
  auto dcStub = MakeStub(&state);
  auto mqttStub = std::make_shared<MQTTManagerProto::MockMQTTManagerServiceStub>();

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(dcStub);
  mgr.setMqttStub(mqttStub);

  std::mutex addMu;
  std::condition_variable addCv;
  std::atomic<int> addCount{0};
  ON_CALL(*mqttStub, RequestAndWait(_, _, _))
      .WillByDefault(::testing::Invoke([&addMu, &addCv, &addCount](
                                           grpc::ClientContext *,
                                           const MQTTManagerProto::RequestAndWaitRequest &req,
                                           MQTTManagerProto::RequestAndWaitResponse *resp) {
        if (resp == nullptr) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
        }
        resp->set_ok(true);
        resp->set_message("成功");
        if (req.request_topic().find("addslaveNode") != std::string::npos) {
          addCount.fetch_add(1);
          addCv.notify_all();
          resp->set_payload("{\"status\":\"Frametimeout\"}");
        } else if (req.request_topic().find("delslaveNode") != std::string::npos) {
          resp->set_payload("{\"status\":0}");
        } else {
          resp->set_payload("{\"status\":0,\"data\":\"AQ==\"}");
        }
        return grpc::Status::OK;
      }));

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c-archive-stop");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-archive-stop", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo linkInfo;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &linkInfo).ok());

  DLT645Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-archive-stop");
  *ptReq.add_points() = MakePointProto("P1", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  auto st = mgr.StartLink("conn-archive-stop");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);

  {
    std::unique_lock<std::mutex> lock(addMu);
    ASSERT_TRUE(addCv.wait_for(lock, std::chrono::seconds(2), [&addCount]() { return addCount.load() >= 1; }));
  }

  DLT645Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-archive-stop", &got).ok());
  EXPECT_EQ(got.state(), DLT645Proto::LINK_STATE_STOPPED);
  EXPECT_EQ(got.last_error(), "档案添加失败: 帧超时");

  ASSERT_TRUE(mgr.StopLink("conn-archive-stop").ok());

  {
    std::unique_lock<std::mutex> lock(addMu);
    EXPECT_FALSE(addCv.wait_for(lock, std::chrono::seconds(6), [&addCount]() { return addCount.load() >= 2; }));
  }
  EXPECT_EQ(addCount.load(), 1);
}

// 验证：UpsertPointTable 在运行中/待删除时拒绝更新。
TEST(Dlt645LinkManagerTest, UpsertPointTableRejectsWhenRunningOrPendingDelete) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-pt", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

  DLT645Proto::UpsertPointTableRequest req;
  req.set_conn_name("conn-pt");
  *req.add_points() = MakePointProto("A", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16);
  req.set_replace(true);

  DLT645LinkManagerTestPeer::SetLinkState(mgr, "conn-pt", DLT645Proto::LINK_STATE_RUNNING);
  auto st = mgr.UpsertPointTable(req);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  DLT645LinkManagerTestPeer::SetLinkState(mgr, "conn-pt", DLT645Proto::LINK_STATE_PENDING_DELETE);
  st = mgr.UpsertPointTable(req);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：UpsertPointTable 点表非法时返回错误。
TEST(Dlt645LinkManagerTest, UpsertPointTableRejectsInvalidPoint) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-bad", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

  DLT645Proto::UpsertPointTableRequest req;
  req.set_conn_name("conn-bad");
  auto *point = req.add_points();
  point->set_tag("A");
  point->set_di("ABC");
  point->set_data_len(2);
  point->set_type(DLT645Proto::DATA_TYPE_UINT16);
  point->set_access(DLT645Proto::ACCESS_READ_ONLY);
  req.set_replace(true);

  auto st = mgr.UpsertPointTable(req);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：解析响应 Base64 失败时返回错误。
TEST(Dlt645LinkManagerTest, ParseResponsePayloadRejectsInvalidBase64) {
  LinkManager mgr("DLT645");
  ParsedFrame frame;
  std::string error;
  auto st = DLT645LinkManagerTestPeer::ParseResponsePayload(mgr, "@@@", &frame, &error);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(error.empty());
}

// 验证：解析响应帧可从报文流提取有效帧。
TEST(Dlt645LinkManagerTest, ParseResponsePayloadExtractsFrameFromStream) {
  LinkManager mgr("DLT645");
  std::vector<uint8_t> addr = DLT645LinkManagerTestPeer::EncodeAddress("123456789012");
  std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
  auto frame = DLT645LinkManagerTestPeer::BuildFrame(addr, 0x91, data);
  std::vector<uint8_t> raw = {0x00, 0x01, 0x02};
  raw.insert(raw.end(), frame.begin(), frame.end());
  raw.push_back(0xFF);
  const auto payloadBase64 = Base64Encode(raw);

  ParsedFrame parsed;
  std::string error;
  auto st = DLT645LinkManagerTestPeer::ParseResponsePayload(mgr, payloadBase64, &parsed, &error);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(parsed.control, 0x91);
  EXPECT_EQ(parsed.data.size(), data.size());
}

// 验证：decodeAndPublish 全 FF 数据返回坏质量并发布默认值。
TEST(Dlt645LinkManagerTest, DecodeAndPublishAllFF) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-ff", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

  PointTable::Point point = MakePoint("A", 2, DLT645Proto::DATA_TYPE_UINT16, 1.0, 0.0, 0.0);
  std::vector<uint8_t> payload = {0xFF, 0xFF};
  auto st = DLT645LinkManagerTestPeer::DecodeAndPublish(mgr, "conn-ff", point, payload, 1, false);
  EXPECT_TRUE(st.ok());

  DataCenterProto::GetLatestRequest req;
  req.set_conn_id(DLT645LinkManagerTestPeer::GetConnId(mgr, "conn-ff"));
  req.add_tags("A");
  DataCenterProto::GetLatestResponse resp;
  ASSERT_TRUE(state.GetLatest(req, &resp).ok());
  ASSERT_EQ(resp.updates_size(), 1);
  EXPECT_EQ(resp.updates(0).quality(), DataCenterProto::QUALITY_BAD);
}

// 验证：decodeAndPublish 支持字符串裁剪与数值死区边界发布。
TEST(Dlt645LinkManagerTest, DecodeAndPublishStringTrimAndDeadband) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-str", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

  PointTable::Point point = MakePoint("S", 4, DLT645Proto::DATA_TYPE_STRING, 1.0, 0.0, 0.1);
  std::vector<uint8_t> payload = {'a', 'b', ' ', ' '};
  auto st = DLT645LinkManagerTestPeer::DecodeAndPublish(mgr, "conn-str", point, payload, 1, true);
  EXPECT_TRUE(st.ok());

  DataCenterProto::GetLatestRequest req;
  req.set_conn_id(DLT645LinkManagerTestPeer::GetConnId(mgr, "conn-str"));
  req.add_tags("S");
  DataCenterProto::GetLatestResponse resp;
  ASSERT_TRUE(state.GetLatest(req, &resp).ok());
  ASSERT_EQ(resp.updates_size(), 1);
  EXPECT_EQ(resp.updates(0).value().string_value(), "ab");

  PointTable::Point floatPoint = MakePoint("F", 4, DLT645Proto::DATA_TYPE_FLOAT, 1.0, 0.0, 1.0);
  float f1 = 10.0f;
  uint32_t u1 = 0;
  std::memcpy(&u1, &f1, sizeof(float));
  std::vector<uint8_t> floatPayload = {static_cast<uint8_t>(u1 & 0xFF), static_cast<uint8_t>((u1 >> 8) & 0xFF), static_cast<uint8_t>((u1 >> 16) & 0xFF), static_cast<uint8_t>((u1 >> 24) & 0xFF)};
  st = DLT645LinkManagerTestPeer::DecodeAndPublish(mgr, "conn-str", floatPoint, floatPayload, 2, false);
  EXPECT_TRUE(st.ok());
  const auto connId = DLT645LinkManagerTestPeer::GetConnId(mgr, "conn-str");
  EXPECT_EQ(state.GetPublishCount(connId, "F"), 1u);

  float f2 = 10.5f;
  uint32_t u2 = 0;
  std::memcpy(&u2, &f2, sizeof(float));
  std::vector<uint8_t> floatPayload2 = {static_cast<uint8_t>(u2 & 0xFF), static_cast<uint8_t>((u2 >> 8) & 0xFF), static_cast<uint8_t>((u2 >> 16) & 0xFF), static_cast<uint8_t>((u2 >> 24) & 0xFF)};
  // 死区过滤：差值小于 deadband 时过滤，刚好等于 deadband 时应上报。
  st = DLT645LinkManagerTestPeer::DecodeAndPublish(mgr, "conn-str", floatPoint, floatPayload2, 3, false);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(state.GetPublishCount(connId, "F"), 1u);

  float f3 = 11.0f;
  uint32_t u3 = 0;
  std::memcpy(&u3, &f3, sizeof(float));
  std::vector<uint8_t> floatPayload3 = {static_cast<uint8_t>(u3 & 0xFF), static_cast<uint8_t>((u3 >> 8) & 0xFF), static_cast<uint8_t>((u3 >> 16) & 0xFF), static_cast<uint8_t>((u3 >> 24) & 0xFF)};
  // 差值刚好等于 deadband，应发布。
  st = DLT645LinkManagerTestPeer::DecodeAndPublish(mgr, "conn-str", floatPoint, floatPayload3, 4, false);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(state.GetPublishCount(connId, "F"), 2u);

  DataCenterProto::GetLatestRequest latestReq;
  latestReq.set_conn_id(connId);
  latestReq.add_tags("F");
  DataCenterProto::GetLatestResponse latestResp;
  ASSERT_TRUE(state.GetLatest(latestReq, &latestResp).ok());
  ASSERT_EQ(latestResp.updates_size(), 1);
  EXPECT_DOUBLE_EQ(latestResp.updates(0).value().double_value(), 11.0);
}

// 验证：decodeAndPublish 支持数值与 BCD 错误分支。
TEST(Dlt645LinkManagerTest, DecodeAndPublishNumericAndBcdErrors) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-num", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

  PointTable::Point u16 = MakePoint("U16", 2, DLT645Proto::DATA_TYPE_UINT16, 1.0, 0.0, 0.0);
  std::vector<uint8_t> payload = {0x10, 0x00};
  auto st = DLT645LinkManagerTestPeer::DecodeAndPublish(mgr, "conn-num", u16, payload, 1, false);
  EXPECT_TRUE(st.ok());

  PointTable::Point bcd = MakePoint("BCD", 1, DLT645Proto::DATA_TYPE_BCD, 1.0, 0.0, 0.0);
  std::vector<uint8_t> bad = {0xFA};
  st = DLT645LinkManagerTestPeer::DecodeAndPublish(mgr, "conn-num", bcd, bad, 1, false);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：decodeAndPublish 按最高位符号规则解析 BCD 负数。
TEST(Dlt645LinkManagerTest, DecodeAndPublishSignedBcdNegativeValue) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  DLT645Proto::UpsertLinkRequest linkReq;
  *linkReq.mutable_config() = MakeValidLinkConfig("conn-signed-bcd", DLT645Proto::COMM_MODE_LORA);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

  PointTable::Point bcd = MakePoint("BCD_NEG", 3, DLT645Proto::DATA_TYPE_BCD, 1.0, 0.0, 0.0);
  std::vector<uint8_t> payload = {0x31, 0x00, 0x80};
  auto st = DLT645LinkManagerTestPeer::DecodeAndPublish(mgr, "conn-signed-bcd", bcd, payload, 1, false);
  ASSERT_TRUE(st.ok());

  DataCenterProto::GetLatestRequest req;
  req.set_conn_id(DLT645LinkManagerTestPeer::GetConnId(mgr, "conn-signed-bcd"));
  req.add_tags("BCD_NEG");
  DataCenterProto::GetLatestResponse resp;
  ASSERT_TRUE(state.GetLatest(req, &resp).ok());
  ASSERT_EQ(resp.updates_size(), 1);
  EXPECT_DOUBLE_EQ(resp.updates(0).value().double_value(), -31.0);
}

// 验证：encodeData 按最高位符号规则编码 BCD 负数。
TEST(Dlt645LinkManagerTest, EncodeDataSignedBcdNegativeValue) {
  PointTable::Point bcd = MakePoint("BCD_NEG", 3, DLT645Proto::DATA_TYPE_BCD, 1.0, 0.0, 0.0);
  DataCenterProto::PointValue value;
  value.set_double_value(-31.0);
  std::string error;

  auto payload = DLT645LinkManagerTestPeer::EncodeData(bcd, value, &error);

  EXPECT_TRUE(error.empty());
  ASSERT_EQ(payload.size(), 3u);
  EXPECT_EQ(payload[0], 0x31);
  EXPECT_EQ(payload[1], 0x00);
  EXPECT_EQ(payload[2], 0x80);
}
