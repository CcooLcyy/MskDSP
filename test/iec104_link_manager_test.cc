#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <filesystem>
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
using ::testing::InSequence;
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

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    path_ = base / ("iec104_link_manager_test_tmp_" + std::to_string(counter_++));
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
}  // 命名空间结束

namespace IEC104 {
class IEC104LinkManagerTestPeer {
public:
  static grpc::Status HandleClientPointValue(LinkManager& mgr, const std::string& connName, const PointValue& pv) {
    return mgr.handleClientPointValue(connName, pv);
  }

  static CommandResult HandleCommandValue(LinkManager& mgr, const std::string& connName, const CommandValue& cv) {
    return mgr.handleCommandValue(connName, cv);
  }

  static grpc::Status HandleTimeSyncCommand(LinkManager& mgr, const std::string& connName, int64_t tsMs) {
    return mgr.handleTimeSyncCommand(connName, tsMs);
  }

  static std::vector<PointValue> BuildInterrogationSnapshot(LinkManager& mgr, const std::string& connName) {
    return mgr.buildInterrogationSnapshot(connName);
  }
};
}  // IEC104 命名空间结束

namespace {
using IEC104::IEC104LinkManagerTestPeer;
}  // 命名空间结束

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

  // 第二条使用相同端口的链路应在任何 DataCenter RPC 之前就被拒绝。
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

// 验证：UpsertPointTable 成功后链路保持 STOPPED，需显式调用 StartLink 才启动链路功能。
TEST(IEC104LinkManagerTest, UpsertPointTableKeepsStoppedUntilExplicitStart) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  auto req = MakeServerLinkReq("conn-auto", "0.0.0.0", AllocateFreeTcpPort());
  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  IEC104Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-auto");
  *ptReq.add_points() = MakePoint("A", 100);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());

  IEC104Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-auto", &got).ok());
  EXPECT_EQ(got.state(), IEC104Proto::LINK_STATE_STOPPED);
  EXPECT_TRUE(got.last_error().empty());

  ASSERT_TRUE(mgr.StartLink("conn-auto").ok());
  ASSERT_TRUE(mgr.GetLink("conn-auto", &got).ok());
  EXPECT_EQ(got.state(), IEC104Proto::LINK_STATE_RUNNING);
  EXPECT_TRUE(got.last_error().empty());

  ASSERT_TRUE(mgr.StopLink("conn-auto").ok());
}

// 验证：运行态链路更新点表会被拒绝，调用方需先停止链路。
TEST(IEC104LinkManagerTest, UpsertPointTableRejectsWhenLinkRunning) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  auto req = MakeServerLinkReq("conn-running", "0.0.0.0", AllocateFreeTcpPort());
  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  IEC104Proto::UpsertPointTableRequest pt1;
  pt1.set_conn_name("conn-running");
  *pt1.add_points() = MakePoint("A", 100);
  pt1.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(pt1).ok());
  ASSERT_TRUE(mgr.StartLink("conn-running").ok());

  EXPECT_CALL(*stub, UpsertConnTags(_, _, _)).Times(0);

  IEC104Proto::UpsertPointTableRequest pt2;
  pt2.set_conn_name("conn-running");
  *pt2.add_points() = MakePoint("B", 101);
  pt2.set_replace(false);

  auto st = mgr.UpsertPointTable(pt2);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(st.error_message(), "更新点表前请先停止链路");

  IEC104Proto::PointTable out;
  ASSERT_TRUE(mgr.GetPointTable("conn-running", &out).ok());
  ASSERT_EQ(out.points_size(), 1);
  EXPECT_EQ(out.points(0).tag(), "A");

  ASSERT_TRUE(mgr.StopLink("conn-running").ok());
}

// 验证：已具备点表的停止态链路执行 UpsertLink 更新后，仍保持 STOPPED。
TEST(IEC104LinkManagerTest, UpsertLinkUpdateKeepsStoppedWhenPointTableReady) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  const auto port = AllocateFreeTcpPort();
  auto createReq = MakeServerLinkReq("conn-update", "0.0.0.0", port);
  IEC104Proto::LinkInfo created;
  ASSERT_TRUE(mgr.UpsertLink(createReq, &created).ok());

  IEC104Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-update");
  *ptReq.add_points() = MakePoint("A", 100);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  ASSERT_TRUE(mgr.StopLink("conn-update").ok());

  auto updateReq = MakeServerLinkReq("conn-update", "0.0.0.0", port);
  updateReq.set_create_only(false);
  updateReq.mutable_config()->set_ca(2);

  IEC104Proto::LinkInfo updated;
  ASSERT_TRUE(mgr.UpsertLink(updateReq, &updated).ok());
  EXPECT_EQ(updated.config().ca(), 2);

  IEC104Proto::LinkInfo got;
  ASSERT_TRUE(mgr.GetLink("conn-update", &got).ok());
  EXPECT_EQ(got.state(), IEC104Proto::LINK_STATE_STOPPED);
  EXPECT_TRUE(got.last_error().empty());
}

// 验证：RenameLink 成功后保留 conn_id，旧名字失效，新名字可继续操作点表与启停。
TEST(IEC104LinkManagerTest, RenameLinkKeepsConnIdAndMovesPointTable) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  const auto port = AllocateFreeTcpPort();
  auto createReq = MakeServerLinkReq("conn-old", "0.0.0.0", port);
  IEC104Proto::LinkInfo created;
  ASSERT_TRUE(mgr.UpsertLink(createReq, &created).ok());

  IEC104Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-old");
  *ptReq.add_points() = MakePoint("A", 100);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  ASSERT_TRUE(mgr.StopLink("conn-old").ok());

  IEC104Proto::LinkInfo renamed;
  ASSERT_TRUE(mgr.RenameLink("conn-old", "conn-new", &renamed).ok());
  EXPECT_EQ(renamed.conn_id(), created.conn_id());
  EXPECT_EQ(renamed.config().conn_name(), "conn-new");
  EXPECT_FALSE(state.HasConnection("IEC104", "conn-old"));
  EXPECT_TRUE(state.HasConnection("IEC104", "conn-new"));

  IEC104Proto::LinkInfo oldInfo;
  auto oldStatus = mgr.GetLink("conn-old", &oldInfo);
  EXPECT_EQ(oldStatus.error_code(), grpc::StatusCode::NOT_FOUND);

  IEC104Proto::ListLinksResponse listResp;
  ASSERT_TRUE(mgr.ListLinks(&listResp).ok());
  ASSERT_EQ(listResp.links_size(), 1);
  EXPECT_EQ(listResp.links(0).config().conn_name(), "conn-new");

  IEC104Proto::PointTable pointTable;
  ASSERT_TRUE(mgr.GetPointTable("conn-new", &pointTable).ok());
  ASSERT_EQ(pointTable.points_size(), 1);
  EXPECT_EQ(pointTable.points(0).tag(), "A");

  IEC104Proto::UpsertPointTableRequest ptUpdateReq;
  ptUpdateReq.set_conn_name("conn-new");
  *ptUpdateReq.add_points() = MakePoint("B", 101);
  ptUpdateReq.set_replace(false);
  ASSERT_TRUE(mgr.UpsertPointTable(ptUpdateReq).ok());

  ASSERT_TRUE(mgr.GetPointTable("conn-new", &pointTable).ok());
  ASSERT_EQ(pointTable.points_size(), 2);
  EXPECT_EQ(pointTable.points(0).tag(), "A");
  EXPECT_EQ(pointTable.points(1).tag(), "B");

  ASSERT_TRUE(mgr.StartLink("conn-new").ok());
  ASSERT_TRUE(mgr.StopLink("conn-new").ok());
  ASSERT_TRUE(mgr.DeleteLink("conn-new").ok());
  EXPECT_FALSE(state.HasConnection("IEC104", "conn-new"));
}

// 验证：RenameLink 在目标名字已存在时返回 ALREADY_EXISTS。
TEST(IEC104LinkManagerTest, RenameLinkRejectsWhenNewConnNameExists) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  IEC104Proto::LinkInfo oldInfo;
  ASSERT_TRUE(mgr.UpsertLink(MakeClientLinkReq("conn-old"), &oldInfo).ok());
  IEC104Proto::LinkInfo newInfo;
  ASSERT_TRUE(mgr.UpsertLink(MakeClientLinkReq("conn-new"), &newInfo).ok());

  IEC104Proto::LinkInfo renamed;
  auto status = mgr.RenameLink("conn-old", "conn-new", &renamed);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：RenameLink 在旧名字不存在时返回 NOT_FOUND。
TEST(IEC104LinkManagerTest, RenameLinkRejectsWhenOldConnNameMissing) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  IEC104Proto::LinkInfo info;
  auto status = mgr.RenameLink("missing", "conn-new", &info);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 验证：运行中的链路不允许 RenameLink。
TEST(IEC104LinkManagerTest, RenameLinkRejectsWhenLinkRunning) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  auto createReq = MakeServerLinkReq("conn-running-rename", "0.0.0.0", AllocateFreeTcpPort());
  IEC104Proto::LinkInfo created;
  ASSERT_TRUE(mgr.UpsertLink(createReq, &created).ok());

  IEC104Proto::UpsertPointTableRequest ptReq;
  ptReq.set_conn_name("conn-running-rename");
  *ptReq.add_points() = MakePoint("A", 100);
  ptReq.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
  ASSERT_TRUE(mgr.StartLink("conn-running-rename").ok());

  IEC104Proto::LinkInfo renamed;
  auto status = mgr.RenameLink("conn-running-rename", "conn-renamed", &renamed);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  ASSERT_TRUE(mgr.StopLink("conn-running-rename").ok());
}

// 验证：链路配置与点表落盘后，新实例恢复时仍会自动恢复链路连接功能。
TEST(IEC104LinkManagerTest, LoadPersistedConfigAutoStartsRestoredReadyLink) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  const auto port = AllocateFreeTcpPort();
  uint32_t connId = 0;
  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(stub);

    auto req = MakeServerLinkReq("conn-persist", "0.0.0.0", port);
    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
    connId = info.conn_id();

    IEC104Proto::UpsertPointTableRequest ptReq;
    ptReq.set_conn_name("conn-persist");
    *ptReq.add_points() = MakePoint("A", 100);
    ptReq.set_replace(true);
    ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
    ASSERT_TRUE(mgr.StopLink("conn-persist").ok());
  }

  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(stub);
    mgr.LoadPersistedConfig();

    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.GetLink("conn-persist", &info).ok());
    EXPECT_EQ(info.conn_id(), connId);
    EXPECT_EQ(info.state(), IEC104Proto::LINK_STATE_RUNNING);
    EXPECT_TRUE(info.last_error().empty());

    IEC104Proto::PointTable pointTable;
    ASSERT_TRUE(mgr.GetPointTable("conn-persist", &pointTable).ok());
    ASSERT_EQ(pointTable.points_size(), 1);
    EXPECT_EQ(pointTable.points(0).tag(), "A");

    ASSERT_TRUE(mgr.StopLink("conn-persist").ok());
  }
}

// 验证：RenameLink 落盘后，新实例恢复时仅保留新名字且点表仍可读取。
TEST(IEC104LinkManagerTest, LoadPersistedConfigKeepsRenamedLink) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  uint32_t connId = 0;
  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(stub);

    auto req = MakeServerLinkReq("conn-old-persist", "0.0.0.0", AllocateFreeTcpPort());
    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());
    connId = info.conn_id();

    IEC104Proto::UpsertPointTableRequest ptReq;
    ptReq.set_conn_name("conn-old-persist");
    *ptReq.add_points() = MakePoint("A", 100);
    ptReq.set_replace(true);
    ASSERT_TRUE(mgr.UpsertPointTable(ptReq).ok());
    ASSERT_TRUE(mgr.RenameLink("conn-old-persist", "conn-new-persist", &info).ok());
    ASSERT_TRUE(mgr.StopLink("conn-new-persist").ok());
  }

  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(stub);
    mgr.LoadPersistedConfig();

    IEC104Proto::LinkInfo info;
    auto oldStatus = mgr.GetLink("conn-old-persist", &info);
    EXPECT_EQ(oldStatus.error_code(), grpc::StatusCode::NOT_FOUND);

    ASSERT_TRUE(mgr.GetLink("conn-new-persist", &info).ok());
    EXPECT_EQ(info.conn_id(), connId);

    IEC104Proto::PointTable pointTable;
    ASSERT_TRUE(mgr.GetPointTable("conn-new-persist", &pointTable).ok());
    ASSERT_EQ(pointTable.points_size(), 1);
    EXPECT_EQ(pointTable.points(0).tag(), "A");

    ASSERT_TRUE(mgr.StopLink("conn-new-persist").ok());
  }
}

// 验证：停止态链路的点表增量更新会合并并同步完整 tags 到 DataCenter。
TEST(IEC104LinkManagerTest, UpsertPointTableMergesAndSendsTagsWhenStopped) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  LinkManager mgr("IEC104");
  mgr.setDataCenterStub(stub);

  auto req = MakeServerLinkReq("conn-pt", "0.0.0.0", AllocateFreeTcpPort());
  IEC104Proto::LinkInfo info;
  ASSERT_TRUE(mgr.UpsertLink(req, &info).ok());

  const auto connId = info.conn_id();
  InSequence seq;
  EXPECT_CALL(*stub, UpsertConnTags(_, _, _))
      .WillOnce(Invoke([connId](grpc::ClientContext*,
                                const DataCenterProto::UpsertConnTagsRequest& req,
                                DataCenterProto::Empty*) {
        EXPECT_EQ(req.conn_id(), connId);
        EXPECT_TRUE(req.replace());
        std::vector<std::string> tags(req.tags().begin(), req.tags().end());
        EXPECT_THAT(tags, ::testing::ElementsAre("A", "B", "__time_sync__"));
        return grpc::Status::OK;
      }));
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

  IEC104Proto::UpsertPointTableRequest pt1;
  pt1.set_conn_name("conn-pt");
  *pt1.add_points() = MakePoint("A", 100);
  *pt1.add_points() = MakePoint("B", 101);
  pt1.set_replace(true);
  ASSERT_TRUE(mgr.UpsertPointTable(pt1).ok());

  ASSERT_TRUE(mgr.StopLink("conn-pt").ok());

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

// 验证：handleCommandValue 在从站时可同步执行设点/遥控。
TEST(IEC104LinkManagerTest, HandleCommandValueExecutesWhenSlave) {
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
  auto result = IEC104LinkManagerTestPeer::HandleCommandValue(mgr, "conn-cmd", cv);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(state.GetCommandCount(info.conn_id(), "F"), 1u);

  cv.ioa = 13;
  cv.type = IEC104Proto::POINT_TYPE_SINGLE;
  cv.boolValue = true;
  result = IEC104LinkManagerTestPeer::HandleCommandValue(mgr, "conn-cmd", cv);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(state.GetCommandCount(info.conn_id(), "C"), 1u);
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
  auto result = IEC104LinkManagerTestPeer::HandleCommandValue(mgr, "conn-master", cv);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(state.GetCommandCount(info.conn_id(), "F"), 0u);
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
