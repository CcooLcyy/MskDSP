#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <thread>
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

// 看门狗执行器：用于验证「必须能返回」的调用没有发生死锁。
// 死锁线程无法回收（它持有着目标对象与互斥量），因此一旦超时就直接打印中文失败信息并以失败码
// 结束测试进程，避免测试自身被永久挂起、把整个 ctest 任务拖死而看不到失败原因。
void RunWithDeadlockWatchdog(const char* what, const std::function<void()>& task,
                             std::chrono::milliseconds timeout) {
  std::promise<void> done;
  auto future = done.get_future();
  std::thread worker([&task, &done]() {
    task();
    done.set_value();
  });

  if (future.wait_for(timeout) == std::future_status::ready) {
    // 任务已返回，先回收线程再让 promise/future 析构，避免与 set_value 竞争共享状态。
    worker.join();
    return;
  }

  worker.detach();
  std::fflush(nullptr);
  std::fprintf(stderr, "\n[死锁检测] %s 在 %lld ms 内未返回，判定为死锁\n", what,
               static_cast<long long>(timeout.count()));
  std::fflush(stderr);
  std::_Exit(EXIT_FAILURE);
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

// 验证：模块启动阶段重载持久化配置、替换已自动启动的运行中链路时不会自锁；
// 重载完成后管理器必须仍能响应 gRPC 接口（ListLinks 同样需要获取内部互斥量）。
TEST(IEC104PersistenceTest, ReloadAfterAutoStartKeepsManagerResponsive) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";

  // 本地监听端口：让客户端链路真正连上，使传输层稳定离开「已断开」初态，
  // 从而确保重载时旧链路析构一定会触发连接状态回调（这是自锁的触发点）。
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context listenIo;
  tcp::acceptor listener(listenIo);
  listener.open(tcp::v4());
  listener.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0));
  listener.listen(1);
  const auto listenPort = listener.local_endpoint().port();
  // 收下并保持对端连接：既排空监听队列（便于重载后链路重连），又避免对端立刻看到 FIN 导致状态回落。
  tcp::socket acceptedPeer(listenIo);

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  {
    LinkManager mgr("IEC104", configDbPath);
    mgr.setDataCenterStub(stub);

    IEC104Proto::LinkInfo info;
    auto linkReq = MakeClientLinkReq("conn-reload");
    linkReq.mutable_config()->mutable_remote()->set_port(listenPort);
    ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

    IEC104Proto::UpsertPointTableRequest pointReq;
    pointReq.set_conn_name("conn-reload");
    pointReq.set_replace(true);
    *pointReq.add_points() = MakePoint("telemetry_a", 100);
    ASSERT_TRUE(mgr.UpsertPointTable(pointReq).ok());
  }

  {
    LinkManager mgr("IEC104", configDbPath);
    // 注入 DataCenter Stub 会触发「设置 Stub 后重试」重载：链路被恢复并自动启动。
    mgr.setDataCenterStub(stub);

    // 等待传输层真正进入「已连接」：只有传输层状态不是「已断开」时，
    // 重载替换链路才会回调 LinkManager，用例才是对该死锁的有效复现。
    IEC104Proto::LinkInfo started;
    bool connected = false;
    const auto connectDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < connectDeadline) {
      ASSERT_TRUE(mgr.GetLink("conn-reload", &started).ok());
      if (started.connection_state() == IEC104Proto::CONNECTION_STATE_CONNECTED) {
        connected = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(connected) << "客户端链路未在超时内进入已连接状态，无法复现启动阶段重载自锁";
    ASSERT_EQ(started.state(), IEC104Proto::LINK_STATE_RUNNING);

    boost::system::error_code acceptEc;
    listener.accept(acceptedPeer, acceptEc);
    if (acceptEc) {
      std::fprintf(stderr, "[用例提示] 收下对端连接失败（不影响死锁复现）: %s\n", acceptEc.message().c_str());
    }

    // 模拟 IEC104::start() 的启动阶段重载：修复前会在持锁状态下析构运行中的链路并永久自锁。
    RunWithDeadlockWatchdog(
        "LoadPersistedConfig", [&mgr]() { mgr.LoadPersistedConfig(); },
        std::chrono::seconds(10));

    IEC104Proto::ListLinksResponse links;
    RunWithDeadlockWatchdog(
        "ListLinks", [&mgr, &links]() { (void)mgr.ListLinks(&links); },
        std::chrono::seconds(10));
    EXPECT_EQ(links.links_size(), 1);

    ASSERT_TRUE(mgr.StopLink("conn-reload").ok());
  }
}
