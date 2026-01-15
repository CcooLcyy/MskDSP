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

#include "DataCenter_mock.grpc.pb.h"
#include "IEC104LinkManager.h"
#include "support/FakeDataCenter.hpp"

namespace {
using IEC104::LinkManager;

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
