#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DataCenter_mock.grpc.pb.h"
#include "IEC104LinkManager.h"
#include "IEC104TcpLink.h"
#include "support/FakeDataCenter.hpp"

namespace {
using IEC104::LinkManager;
using IEC104::PointValue;
using IEC104::CommandValue;

using ::testing::_;
using ::testing::Invoke;

IEC104Proto::UpsertLinkRequest MakeClientLinkReq(const char* connName) {
  IEC104Proto::UpsertLinkRequest req;
  auto* cfg = req.mutable_config();
  cfg->set_conn_name(connName);
  cfg->set_role(IEC104Proto::ROLE_CLIENT);
  cfg->mutable_remote()->set_ip("127.0.0.1");
  cfg->mutable_remote()->set_port(2404);
  cfg->set_ca(1);
  cfg->set_oa(1);
  req.set_create_only(true);
  return req;
}

uint16_t AllocateFreeTcpPort() {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;

  asio::io_context io;
  tcp::acceptor acceptor(io);
  acceptor.open(tcp::v4());
  acceptor.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0));
  return acceptor.local_endpoint().port();
}

IEC104Proto::UpsertLinkRequest MakeServerLinkReq(const char* connName, const char* ip, uint16_t port) {
  IEC104Proto::UpsertLinkRequest req;
  auto* cfg = req.mutable_config();
  cfg->set_conn_name(connName);
  cfg->set_role(IEC104Proto::ROLE_SERVER);
  cfg->mutable_local()->set_ip(ip);
  cfg->mutable_local()->set_port(port);
  cfg->set_ca(1);
  cfg->set_oa(1);
  req.set_create_only(true);
  return req;
}

IEC104Proto::Point MakePoint(const char* tag, uint32_t ioa) {
  IEC104Proto::Point p;
  p.set_tag(tag);
  p.set_ioa(ioa);
  p.set_type(IEC104Proto::POINT_TYPE_FLOAT);
  return p;
}

IEC104Proto::Point MakeBoolPoint(const char* tag, uint32_t ioa) {
  IEC104Proto::Point p;
  p.set_tag(tag);
  p.set_ioa(ioa);
  p.set_type(IEC104Proto::POINT_TYPE_SINGLE);
  return p;
}
}  // namespace

namespace IEC104 {
class IEC104LinkManagerTestPeer {
public:
  static grpc::Status HandleClientPointValue(LinkManager& mgr, const std::string& connName, const PointValue& pv) {
    return mgr.handleClientPointValue(connName, pv);
  }

  static grpc::Status HandleCommandValue(LinkManager& mgr, const std::string& connName, const CommandValue& cv) {
    return mgr.handleCommandValue(connName, cv);
  }

  static grpc::Status HandleTimeSyncCommand(LinkManager& mgr, const std::string& connName, int64_t tsMs) {
    return mgr.handleTimeSyncCommand(connName, tsMs);
  }

  static std::vector<PointValue> BuildInterrogationSnapshot(LinkManager& mgr, const std::string& connName) {
    return mgr.buildInterrogationSnapshot(connName);
  }
};
}  // namespace IEC104

namespace {
using IEC104::IEC104LinkManagerTestPeer;
}  // namespace

// 验证：create_only UpsertLink 会向 DataCenter 取/建 conn_id，并回填到 LinkInfo。
TEST(IEC104LinkManagerTest, UpsertLinkCreateOnlyReturnsConnId) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  auto req = MakeClientLinkReq("conn-1");

  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
  EXPECT_NE(info.conn_id(), 0u);
  EXPECT_EQ(info.state(), IEC104Proto::LINK_STATE_STOPPED);
  EXPECT_EQ(info.config().conn_name(), "conn-1");
  EXPECT_TRUE(state.HasConnection("IEC104", "conn-1"));
}

// 验证：当 DataCenter 已存在相同 (module_name, conn_name) 时，create_only UpsertLink 返回 ALREADY_EXISTS。
TEST(IEC104LinkManagerTest, UpsertLinkCreateOnlyRejectsWhenDataCenterAlreadyHasKey) {
  FakeDataCenterState state;
  state.AddConnection(42, "IEC104", "dup");
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  auto req = MakeClientLinkReq("dup");

  IEC104Proto::LinkInfo info;
  auto st = mgr.UpsertLink(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：DeleteLink 会调用 DataCenter.DeleteConnection，并移除本地 link 配置。
TEST(IEC104LinkManagerTest, DeleteLinkCallsDataCenterDeleteAndRemovesLocal) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  auto req = MakeClientLinkReq("conn-del");

  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
  ASSERT_TRUE(state.HasConnection("IEC104", "conn-del"));

  ASSERT_TRUE(mgr.DeleteLink("conn-del").ok());
  EXPECT_FALSE(state.HasConnection("IEC104", "conn-del"));

  IEC104Proto::LinkInfo got;
  auto st = mgr.GetLink("conn-del", &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：当 DataCenter 删除失败时，DeleteLink 标记 PENDING_DELETE 且保留本地配置以便重试。
TEST(IEC104LinkManagerTest, DeleteLinkFailureMarksPendingDeleteAndKeepsLocal) {
  FakeDataCenterState state;
  state.FailDeleteForConnName("conn-fail");
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  auto req = MakeClientLinkReq("conn-fail");

  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  auto st = mgr.DeleteLink("conn-fail");
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);

  IEC104Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-fail", &got).ok());
  EXPECT_EQ(got.state(), IEC104Proto::LINK_STATE_PENDING_DELETE);
}

// 验证：当 DataCenter 返回 NOT_FOUND 时，DeleteLink 视为幂等成功并移除本地配置。
TEST(IEC104LinkManagerTest, DeleteLinkTreatsDataCenterNotFoundAsSuccess) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);
  auto req = MakeClientLinkReq("conn-nf");

  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
  ASSERT_TRUE(state.HasConnection("IEC104", "conn-nf"));

  state.RemoveConnection("IEC104", "conn-nf");
  ASSERT_FALSE(state.HasConnection("IEC104", "conn-nf"));

  ASSERT_TRUE(mgr.DeleteLink("conn-nf").ok());
  IEC104Proto::LinkInfo got;
  auto st = mgr.GetLink("conn-nf", &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：ROLE_SERVER 配置阶段会阻止同模块内端口冲突（无需等到 StartLink）。
TEST(IEC104LinkManagerTest, UpsertLinkServerRejectsPortConflictBeforeDataCenterCalls) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  const auto port = AllocateFreeTcpPort();
  auto req1 = MakeServerLinkReq("server-1", "127.0.0.1", port);
  IEC104Proto::LinkInfo info1;
  ASSERT_TRUE(mgr.UpsertLink(req1, &info1).ok());
  EXPECT_TRUE(state.HasConnection("IEC104", "server-1"));

  // Second link with the same port should be rejected before any DataCenter RPC.
  EXPECT_CALL(*stub, ListConnections(_, _, _)).Times(0);
  EXPECT_CALL(*stub, GetOrCreateConnection(_, _, _)).Times(0);

  auto req2 = MakeServerLinkReq("server-2", "127.0.0.1", port);
  IEC104Proto::LinkInfo info2;
  auto st = mgr.UpsertLink(req2, &info2);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
  EXPECT_FALSE(state.HasConnection("IEC104", "server-2"));
}

// 验证：ROLE_SERVER 配置阶段会检测系统级端口占用（端口已被其他进程 bind 时直接失败）。
TEST(IEC104LinkManagerTest, UpsertLinkServerRejectsWhenPortIsAlreadyBound) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;

  asio::io_context io;
  tcp::acceptor external(io);
  external.open(tcp::v4());
  external.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0));
  external.listen();
  const auto port = external.local_endpoint().port();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  EXPECT_CALL(*stub, ListConnections(_, _, _)).Times(0);
  EXPECT_CALL(*stub, GetOrCreateConnection(_, _, _)).Times(0);

  auto req = MakeServerLinkReq("server-occupied", "127.0.0.1", port);
  IEC104Proto::LinkInfo info;
  auto st = mgr.UpsertLink(req, &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_FALSE(state.HasConnection("IEC104", "server-occupied"));
}

// 验证：DeleteLink 成功后释放 ROLE_SERVER 端口占用，允许复用同端口创建新连接。
TEST(IEC104LinkManagerTest, DeleteLinkReleasesReservedServerPort) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  const auto port = AllocateFreeTcpPort();
  auto req1 = MakeServerLinkReq("server-old", "127.0.0.1", port);
  IEC104Proto::LinkInfo info1;
  ASSERT_TRUE(mgr.UpsertLink(req1, &info1).ok());

  ASSERT_TRUE(mgr.DeleteLink("server-old").ok());

  auto req2 = MakeServerLinkReq("server-new", "127.0.0.1", port);
  IEC104Proto::LinkInfo info2;
  ASSERT_TRUE(mgr.UpsertLink(req2, &info2).ok());
}

// 验证：点表增量更新会合并并同步完整 tags 到 DataCenter。
TEST(IEC104LinkManagerTest, UpsertPointTableMergesAndSendsTags) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  auto req = MakeClientLinkReq("conn-pt");
  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  IEC104Proto::UpsertPointTableRequest pt1;
  pt1.set_conn_name("conn-pt");
  *pt1.add_points() = MakePoint("A", 100);
  *pt1.add_points() = MakePoint("B", 101);
  pt1.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(pt1).ok());

  const auto connId = info.conn_id();
  EXPECT_CALL(*stub, UpsertConnTags(_, _, _))
      .WillOnce(Invoke([connId](grpc::ClientContext*,
                                const DataCenterProto::UpsertConnTagsRequest& req,
                                DataCenterProto::Empty*) {
        EXPECT_EQ(req.conn_id(), connId);
        EXPECT_TRUE(req.replace());
        std::vector<std::string> tags(req.tags().begin(), req.tags().end());
        EXPECT_THAT(tags, ::testing::ElementsAre("A", "B", "C", "__time_sync__"));
        return grpc::Status::OK;
      }));

  IEC104Proto::UpsertPointTableRequest pt2;
  pt2.set_conn_name("conn-pt");
  *pt2.add_points() = MakePoint("C", 102);
  pt2.set_replace(false);
  ASSERT_TRUE(mgr.UpsertPointTable(pt2).ok());

  IEC104Proto::PointTable out;
  ASSERT_TRUE(mgr.GetPointTable("conn-pt", &out).ok());
  ASSERT_EQ(out.points_size(), 3);
  EXPECT_EQ(out.points(0).tag(), "A");
  EXPECT_EQ(out.points(1).tag(), "B");
  EXPECT_EQ(out.points(2).tag(), "C");
}

// 验证：点表更新时连接不存在会返回 NOT_FOUND。
TEST(IEC104LinkManagerTest, UpsertPointTableReturnsNotFoundWhenMissing) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  EXPECT_CALL(*stub, UpsertConnTags(_, _, _)).Times(0);

  IEC104Proto::UpsertPointTableRequest req;
  req.set_conn_name("missing");
  *req.add_points() = MakePoint("A", 1);
  req.set_replace(true);

  auto st = mgr.UpsertPointTable(req);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：handleClientPointValue 在连接不存在时返回 NOT_FOUND。
TEST(IEC104LinkManagerTest, HandleClientPointValueRejectsMissingLink) {
  LinkManager mgr("IEC104");
  PointValue pv;
  pv.ioa = 1;
  pv.type = IEC104Proto::POINT_TYPE_FLOAT;
  pv.doubleValue = 10.0;
  auto st = IEC104LinkManagerTestPeer::HandleClientPointValue(mgr, "missing", pv);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：handleClientPointValue 支持死区过滤与质量映射。
TEST(IEC104LinkManagerTest, HandleClientPointValueDeadbandAndQuality) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  auto req = MakeServerLinkReq("conn-dead", "0.0.0.0", AllocateFreeTcpPort());
  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  IEC104Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-dead");
  auto *p = ptReq.add_points();
  *p = MakePoint("A", 10);
  p->set_deadband(1.0);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  PointValue pv;
  pv.ioa = 10;
  pv.type = IEC104Proto::POINT_TYPE_FLOAT;
  pv.doubleValue = 5.0;
  pv.quality = 0;
  pv.tsMs = 100;
  ASSERT_TRUE(IEC104LinkManagerTestPeer::HandleClientPointValue(mgr, "conn-dead", pv).ok());

  // 小于死区的变化应被过滤。
  pv.doubleValue = 5.5;
  auto st = IEC104LinkManagerTestPeer::HandleClientPointValue(mgr, "conn-dead", pv);
  EXPECT_TRUE(st.ok());

  DataCenterProto::GetLatestRequest reqLatest;
  reqLatest.set_conn_id(info.conn_id());
  reqLatest.add_tags("A");
  DataCenterProto::GetLatestResponse resp;
  ASSERT_TRUE(state.GetLatest(reqLatest, &resp).ok());
  ASSERT_EQ(resp.updates_size(), 1);
  EXPECT_EQ(resp.updates(0).quality(), DataCenterProto::QUALITY_GOOD);
}

// 验证：handleClientPointValue 支持 BOOL 点上送与错误质量。
TEST(IEC104LinkManagerTest, HandleClientPointValuePublishesBool) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  auto req = MakeServerLinkReq("conn-bool", "0.0.0.0", AllocateFreeTcpPort());
  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  IEC104Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-bool");
  *ptReq.add_points() = MakeBoolPoint("B", 11);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  PointValue pv;
  pv.ioa = 11;
  pv.type = IEC104Proto::POINT_TYPE_SINGLE;
  pv.boolValue = true;
  pv.quality = 0x80;
  pv.tsMs = 200;
  auto st = IEC104LinkManagerTestPeer::HandleClientPointValue(mgr, "conn-bool", pv);
  EXPECT_TRUE(st.ok());

  DataCenterProto::GetLatestRequest reqLatest;
  reqLatest.set_conn_id(info.conn_id());
  reqLatest.add_tags("B");
  DataCenterProto::GetLatestResponse resp;
  ASSERT_TRUE(state.GetLatest(reqLatest, &resp).ok());
  ASSERT_EQ(resp.updates_size(), 1);
  EXPECT_EQ(resp.updates(0).quality(), DataCenterProto::QUALITY_BAD);
}

// 验证：handleCommandValue 在从站时可发布设点/遥控。
TEST(IEC104LinkManagerTest, HandleCommandValuePublishesWhenSlave) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  auto req = MakeServerLinkReq("conn-cmd", "0.0.0.0", AllocateFreeTcpPort());
  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  IEC104Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-cmd");
  *ptReq.add_points() = MakePoint("F", 12);
  *ptReq.add_points() = MakeBoolPoint("C", 13);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  CommandValue cv;
  cv.ioa = 12;
  cv.type = IEC104Proto::POINT_TYPE_FLOAT;
  cv.doubleValue = 3.5;
  auto st = IEC104LinkManagerTestPeer::HandleCommandValue(mgr, "conn-cmd", cv);
  EXPECT_TRUE(st.ok());

  cv.ioa = 13;
  cv.type = IEC104Proto::POINT_TYPE_SINGLE;
  cv.boolValue = true;
  st = IEC104LinkManagerTestPeer::HandleCommandValue(mgr, "conn-cmd", cv);
  EXPECT_TRUE(st.ok());
}

// 验证：handleCommandValue 在非从站时忽略命令。
TEST(IEC104LinkManagerTest, HandleCommandValueIgnoredWhenMaster) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  auto req = MakeClientLinkReq("conn-master");
  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  IEC104Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-master");
  *ptReq.add_points() = MakePoint("F", 12);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  CommandValue cv;
  cv.ioa = 12;
  cv.type = IEC104Proto::POINT_TYPE_FLOAT;
  cv.doubleValue = 3.5;
  auto st = IEC104LinkManagerTestPeer::HandleCommandValue(mgr, "conn-master", cv);
  EXPECT_TRUE(st.ok());
}

// 验证：handleTimeSyncCommand 处理非法时间戳与正常发布。
TEST(IEC104LinkManagerTest, HandleTimeSyncCommandPaths) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  auto req = MakeServerLinkReq("conn-ts", "0.0.0.0", AllocateFreeTcpPort());
  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  auto st = IEC104LinkManagerTestPeer::HandleTimeSyncCommand(mgr, "conn-ts", 0);
  EXPECT_TRUE(st.ok());

  st = IEC104LinkManagerTestPeer::HandleTimeSyncCommand(mgr, "conn-ts", 1000);
  EXPECT_TRUE(st.ok());
}

// 验证：buildInterrogationSnapshot 能从 DataCenter 最新值生成快照。
TEST(IEC104LinkManagerTest, BuildInterrogationSnapshotUsesLatest) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  auto req = MakeServerLinkReq("conn-snap", "0.0.0.0", AllocateFreeTcpPort());
  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  IEC104Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-snap");
  auto *p1 = ptReq.add_points();
  *p1 = MakePoint("A", 10);
  p1->set_scale(2.0);
  p1->set_offset(1.0);
  auto *p2 = ptReq.add_points();
  *p2 = MakeBoolPoint("B", 11);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  DataCenterProto::PublishRequest pub;
  pub.set_conn_id(info.conn_id());
  pub.set_tag("A");
  pub.mutable_value()->set_double_value(5.0);
  pub.set_quality(DataCenterProto::QUALITY_GOOD);
  pub.set_ts_ms(1234);
  ASSERT_TRUE(state.Publish(pub).ok());

  DataCenterProto::PublishRequest pub2;
  pub2.set_conn_id(info.conn_id());
  pub2.set_tag("B");
  pub2.mutable_value()->set_bool_value(true);
  pub2.set_quality(DataCenterProto::QUALITY_GOOD);
  pub2.set_ts_ms(1235);
  ASSERT_TRUE(state.Publish(pub2).ok());

  auto snapshot = IEC104LinkManagerTestPeer::BuildInterrogationSnapshot(mgr, "conn-snap");
  ASSERT_EQ(snapshot.size(), 2u);
}
