#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "IEC104LinkManager.h"
#include "support/FakeDataCenter.hpp"

namespace {
using IEC104::LinkManager;
using ::testing::AtLeast;
using ::testing::Invoke;

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    path_ = base / ("iec104_persistence_test_tmp_" + std::to_string(ts));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

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

IEC104Proto::UpsertLinkRequest MakeServerLinkReq(const char* connName, uint16_t port) {
  IEC104Proto::UpsertLinkRequest req;
  auto* cfg = req.mutable_config();
  cfg->set_conn_name(connName);
  cfg->set_role(IEC104Proto::ROLE_SERVER);
  cfg->mutable_local()->set_ip("127.0.0.1");
  cfg->mutable_local()->set_port(port);
  cfg->set_ca(1);
  cfg->set_oa(1);
  req.set_create_only(true);
  return req;
}

IEC104Proto::Point MakePoint(const char* tag, uint32_t ioa) {
  IEC104Proto::Point point;
  point.set_tag(tag);
  point.set_ioa(ioa);
  point.set_type(IEC104Proto::POINT_TYPE_FLOAT);
  return point;
}

}  // 命名空间结束

// 验证：链路配置与点表在落盘后可被新 LinkManager 实例恢复，且恢复后会自动启动链路功能。
TEST(IEC104PersistenceTest, LoadsPersistedLinkAndPointTableAfterRestart) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";
  const auto port = AllocateFreeTcpPort();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  uint32_t connId = 0;
  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(stub);

    auto linkReq = MakeServerLinkReq("conn-persist", port);
    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());
    connId = info.conn_id();

    IEC104Proto::UpsertPointTableRequest pointReq;
    pointReq.set_conn_name("conn-persist");
    pointReq.set_replace(true);
    *pointReq.add_points() = MakePoint("telemetry_a", 100);
    ASSERT_TRUE(mgr.UpsertPointTable(pointReq).ok());
  }

  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(stub);

    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.GetLink("conn-persist", &info).ok());
    EXPECT_EQ(info.conn_id(), connId);
    EXPECT_EQ(info.state(), IEC104Proto::LINK_STATE_RUNNING);
    EXPECT_EQ(info.config().station_role(), IEC104Proto::STATION_ROLE_SLAVE);
    EXPECT_EQ(info.config().time_sync_tag(), "__time_sync__");
    EXPECT_TRUE(info.last_error().empty());

    IEC104Proto::PointTable pointTable;
    ASSERT_TRUE(mgr.GetPointTable("conn-persist", &pointTable).ok());
    ASSERT_EQ(pointTable.points_size(), 1);
    EXPECT_EQ(pointTable.points(0).tag(), "telemetry_a");
    EXPECT_EQ(pointTable.points(0).ioa(), 100u);

    ASSERT_TRUE(mgr.StopLink("conn-persist").ok());
  }
}

// 验证：恢复配置时保留不可用或重复的服务端监听端点及其点表，启动失败不得清理配置。
TEST(IEC104PersistenceTest, PreservesUnavailableDuplicateServerEndpointsAfterRestart) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";
  const auto port = AllocateFreeTcpPort();

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(stub);

    for (const auto &connName : {"conn-unavailable-a", "conn-unavailable-b"}) {
      auto linkReq = MakeServerLinkReq(connName, port);
      linkReq.mutable_config()->mutable_local()->set_ip("192.0.2.1");
      IEC104Proto::LinkInfo info;
      ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

      IEC104Proto::UpsertPointTableRequest pointReq;
      pointReq.set_conn_name(connName);
      pointReq.set_replace(true);
      *pointReq.add_points() = MakePoint((std::string(connName) + "-point").c_str(), 100);
      ASSERT_TRUE(mgr.UpsertPointTable(pointReq).ok());
    }
  }

  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(stub);

    for (const auto &connName : {"conn-unavailable-a", "conn-unavailable-b"}) {
      IEC104Proto::LinkInfo info;
      ASSERT_TRUE(mgr.GetLink(connName, &info).ok());
      EXPECT_EQ(info.config().local().ip(), "192.0.2.1");
      EXPECT_EQ(info.state(), IEC104Proto::LINK_STATE_STOPPED);
      EXPECT_FALSE(info.last_error().empty());

      IEC104Proto::PointTable pointTable;
      ASSERT_TRUE(mgr.GetPointTable(connName, &pointTable).ok());
      ASSERT_EQ(pointTable.points_size(), 1);
      EXPECT_EQ(pointTable.points(0).ioa(), 100u);
    }
  }
}

// 验证：DeleteLink 进入 PENDING_DELETE 后会落盘，重启后仍阻止启动链路功能。
TEST(IEC104PersistenceTest, LoadsPendingDeleteStateAfterRestart) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";

  FakeDataCenterState state;
  state.FailDeleteForConnName("conn-pending");
  auto stub = MakeStub(&state);

  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(stub);

    auto linkReq = MakeClientLinkReq("conn-pending");
    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

    auto status = mgr.DeleteLink("conn-pending");
    ASSERT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  }

  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(stub);

    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.GetLink("conn-pending", &info).ok());
    EXPECT_EQ(info.state(), IEC104Proto::LINK_STATE_PENDING_DELETE);
    EXPECT_TRUE(info.last_error().empty());

    auto status = mgr.StartLink("conn-pending");
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  }
}

// 验证：恢复过程中若单条链路获取 DataCenter 连接失败，不会中断其他链路恢复。
TEST(IEC104PersistenceTest, ContinuesRestoringOtherLinksWhenSingleLinkFails) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";
  const auto goodPort = AllocateFreeTcpPort();
  const auto badPort = AllocateFreeTcpPort();

  FakeDataCenterState initialState;
  auto initialStub = MakeStub(&initialState);

  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(initialStub);

    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(MakeServerLinkReq("conn-good", goodPort), &info).ok());
    ASSERT_TRUE(mgr.UpsertLink(MakeServerLinkReq("conn-bad", badPort), &info).ok());

    IEC104Proto::UpsertPointTableRequest pointReq;
    pointReq.set_conn_name("conn-good");
    pointReq.set_replace(true);
    *pointReq.add_points() = MakePoint("telemetry_good", 100);
    ASSERT_TRUE(mgr.UpsertPointTable(pointReq).ok());
  }

  FakeDataCenterState recoveredState;
  auto recoveredStub = MakeStub(&recoveredState);
  EXPECT_CALL(*recoveredStub, GetOrCreateConnection(::testing::_, ::testing::_, ::testing::_))
      .Times(AtLeast(2))
      .WillRepeatedly(Invoke([&recoveredState](grpc::ClientContext*,
                                               const DataCenterProto::GetOrCreateConnectionRequest& req,
                                               DataCenterProto::ConnectionInfo* resp) {
        if (req.key().conn_name() == "conn-bad") {
          return grpc::Status(grpc::StatusCode::INTERNAL, "强制获取连接失败");
        }
        return recoveredState.GetOrCreateConnection(req, resp);
      }));

  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(recoveredStub);

    IEC104Proto::LinkInfo good;
    ASSERT_TRUE(mgr.GetLink("conn-good", &good).ok());
    EXPECT_EQ(good.state(), IEC104Proto::LINK_STATE_RUNNING);

    IEC104Proto::PointTable pointTable;
    ASSERT_TRUE(mgr.GetPointTable("conn-good", &pointTable).ok());
    ASSERT_EQ(pointTable.points_size(), 1);
    EXPECT_EQ(pointTable.points(0).tag(), "telemetry_good");

    IEC104Proto::LinkInfo bad;
    auto status = mgr.GetLink("conn-bad", &bad);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);

    ASSERT_TRUE(mgr.StopLink("conn-good").ok());
  }
}

// 验证：当 DataCenter 重新分配 conn_id 时，恢复后的链路会改用新 conn_id，并回写 SQLite 链路配置。
TEST(IEC104PersistenceTest, ReloadsWithReassignedConnIdFromDataCenter) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";
  const auto port = AllocateFreeTcpPort();

  FakeDataCenterState initialState;
  auto initialStub = MakeStub(&initialState);

  uint32_t oldConnId = 0;
  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(initialStub);

    auto linkReq = MakeServerLinkReq("conn-reassigned", port);
    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());
    oldConnId = info.conn_id();

    IEC104Proto::UpsertPointTableRequest pointReq;
    pointReq.set_conn_name("conn-reassigned");
    pointReq.set_replace(true);
    *pointReq.add_points() = MakePoint("telemetry_a", 100);
    ASSERT_TRUE(mgr.UpsertPointTable(pointReq).ok());
  }

  FakeDataCenterState recoveredState;
  recoveredState.SetNextConnId(oldConnId + 10);
  auto recoveredStub = MakeStub(&recoveredState);
  std::vector<uint32_t> syncedConnIds;
  EXPECT_CALL(*recoveredStub, UpsertConnTags(::testing::_, ::testing::_, ::testing::_))
      .Times(AtLeast(2))
      .WillRepeatedly(Invoke([&syncedConnIds](grpc::ClientContext*,
                                              const DataCenterProto::UpsertConnTagsRequest& req,
                                              DataCenterProto::Empty*) {
        syncedConnIds.push_back(req.conn_id());
        if (req.conn_id() == 0) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
        }
        for (const auto& tag : req.tags()) {
          if (tag.empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "tags 包含空字符串");
          }
        }
        return grpc::Status::OK;
      }));

  uint32_t newConnId = 0;
  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(recoveredStub);

    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.GetLink("conn-reassigned", &info).ok());
    newConnId = info.conn_id();
    EXPECT_NE(newConnId, oldConnId);
    EXPECT_EQ(newConnId, oldConnId + 10);
    EXPECT_EQ(info.state(), IEC104Proto::LINK_STATE_RUNNING);
    ASSERT_FALSE(syncedConnIds.empty());
    EXPECT_EQ(syncedConnIds.front(), newConnId);

    ASSERT_TRUE(mgr.StopLink("conn-reassigned").ok());

    IEC104Proto::UpsertPointTableRequest pointReq;
    pointReq.set_conn_name("conn-reassigned");
    pointReq.set_replace(false);
    *pointReq.add_points() = MakePoint("telemetry_b", 101);
    ASSERT_TRUE(mgr.UpsertPointTable(pointReq).ok());
    ASSERT_GE(syncedConnIds.size(), 2u);
    EXPECT_EQ(syncedConnIds.back(), newConnId);
  }

  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(recoveredStub);

    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.GetLink("conn-reassigned", &info).ok());
    EXPECT_EQ(info.conn_id(), newConnId);
    ASSERT_TRUE(mgr.StopLink("conn-reassigned").ok());
  }
}
