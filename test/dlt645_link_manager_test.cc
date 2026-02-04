#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
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
  static grpc::Status ParseResponsePayload(LinkManager &mgr,
                                           const std::string &payloadBase64,
                                           ParsedFrame *out,
                                           std::string *error) {
    LinkManager::Frame frame;
    auto st = mgr.parseResponsePayload(payloadBase64, &frame, error);
    if (st.ok() && out != nullptr) {
      out->address = frame.address;
      out->control = frame.control;
      out->data = frame.data;
    }
    return st;
  }

  static grpc::Status DecodeAndPublish(LinkManager &mgr,
                                       const std::string &connName,
                                       const PointTable::Point &point,
                                       const std::vector<uint8_t> &payload,
                                       int64_t tsMs,
                                       bool trimRightSpace) {
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

  static std::vector<uint8_t> EncodeAddress(const std::string &addr) {
    return LinkManager::encodeAddress(addr);
  }

  static std::vector<uint8_t> BuildFrame(const std::vector<uint8_t> &addr,
                                         uint8_t control,
                                         const std::vector<uint8_t> &data) {
    return LinkManager::buildFrame(addr, control, data);
  }

  static void AddOffset33(std::vector<uint8_t> *data) {
    LinkManager::addOffset33(data);
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

PointTable::Point MakePoint(const char *tag,
                            uint32_t dataLen,
                            DLT645Proto::DataType type,
                            double scale,
                            double offset,
                            double deadband) {
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

  auto st = mgr.StartLink("conn-start");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：StartLink 在运行中/待删除时拒绝启动。
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
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  DLT645LinkManagerTestPeer::SetLinkState(mgr, "conn-running", DLT645Proto::LINK_STATE_PENDING_DELETE);
  st = mgr.StartLink("conn-running");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：StartLink 串口模式返回未实现。
TEST(Dlt645LinkManagerTest, StartLinkSerialModeUnimplemented) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("DLT645");
  mgr.setDataCenterStub(stub);

  auto updateReq = MakeMqttUpdateRequest("127.0.0.1", 1883, "c1");
  DLT645Proto::UpdateConfigResponse updateResp;
  ASSERT_TRUE(mgr.UpdateConfig(updateReq, &updateResp).ok());

  DLT645Proto::UpsertLinkRequest req;
  *req.mutable_config() = MakeValidLinkConfig("conn-serial", DLT645Proto::COMM_MODE_SERIAL);
  DLT645Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  auto st = mgr.StartLink("conn-serial");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::UNIMPLEMENTED);
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

// 验证：decodeAndPublish 支持字符串裁剪与数值死区过滤。
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
  std::vector<uint8_t> floatPayload = {static_cast<uint8_t>(u1 & 0xFF),
                                       static_cast<uint8_t>((u1 >> 8) & 0xFF),
                                       static_cast<uint8_t>((u1 >> 16) & 0xFF),
                                       static_cast<uint8_t>((u1 >> 24) & 0xFF)};
  st = DLT645LinkManagerTestPeer::DecodeAndPublish(mgr, "conn-str", floatPoint, floatPayload, 2, false);
  EXPECT_TRUE(st.ok());

  float f2 = 10.5f;
  uint32_t u2 = 0;
  std::memcpy(&u2, &f2, sizeof(float));
  std::vector<uint8_t> floatPayload2 = {static_cast<uint8_t>(u2 & 0xFF),
                                        static_cast<uint8_t>((u2 >> 8) & 0xFF),
                                        static_cast<uint8_t>((u2 >> 16) & 0xFF),
                                        static_cast<uint8_t>((u2 >> 24) & 0xFF)};
  // 死区过滤：差值小于 deadband 时直接返回 OK。
  st = DLT645LinkManagerTestPeer::DecodeAndPublish(mgr, "conn-str", floatPoint, floatPayload2, 3, false);
  EXPECT_TRUE(st.ok());
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
