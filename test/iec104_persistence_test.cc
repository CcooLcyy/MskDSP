#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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

IEC104Proto::Point MakePoint(const char* tag, uint32_t ioa) {
  IEC104Proto::Point point;
  point.set_tag(tag);
  point.set_ioa(ioa);
  point.set_type(IEC104Proto::POINT_TYPE_FLOAT);
  return point;
}

bool LoadLinksConfigFromFile(const std::filesystem::path& path, IEC104Proto::LinksConfig* out) {
  if (out == nullptr) {
    return false;
  }
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs.is_open()) {
    return false;
  }
  std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  if (!ifs.good() && !ifs.eof()) {
    return false;
  }
  out->Clear();
  return out->ParseFromString(data);
}
}  // 命名空间结束

// 验证：链路配置与点表在落盘后可被新 LinkManager 实例恢复，且恢复后保持 STOPPED。
TEST(IEC104PersistenceTest, LoadsPersistedLinkAndPointTableAfterRestart) {
  ScopedTempDir dir;
  const auto linksPath = dir.path() / "links.pb";
  const auto pointTablesPath = dir.path() / "point_tables.pb";

  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  uint32_t connId = 0;
  {
    LinkManager mgr("IEC104", linksPath, pointTablesPath);
    mgr.setDataCenterStub(stub);

    auto linkReq = MakeClientLinkReq("conn-persist");
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
    LinkManager mgr("IEC104", linksPath, pointTablesPath);
    mgr.setDataCenterStub(stub);

    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.GetLink("conn-persist", &info).ok());
    EXPECT_EQ(info.conn_id(), connId);
    EXPECT_EQ(info.state(), IEC104Proto::LINK_STATE_STOPPED);
    EXPECT_EQ(info.config().station_role(), IEC104Proto::STATION_ROLE_MASTER);
    EXPECT_EQ(info.config().time_sync_tag(), "__time_sync__");
    EXPECT_TRUE(info.last_error().empty());

    IEC104Proto::PointTable pointTable;
    ASSERT_TRUE(mgr.GetPointTable("conn-persist", &pointTable).ok());
    ASSERT_EQ(pointTable.points_size(), 1);
    EXPECT_EQ(pointTable.points(0).tag(), "telemetry_a");
    EXPECT_EQ(pointTable.points(0).ioa(), 100u);
  }
}

// 验证：DeleteLink 进入 PENDING_DELETE 后会落盘，重启后仍阻止启动链路功能。
TEST(IEC104PersistenceTest, LoadsPendingDeleteStateAfterRestart) {
  ScopedTempDir dir;
  const auto linksPath = dir.path() / "links.pb";
  const auto pointTablesPath = dir.path() / "point_tables.pb";

  FakeDataCenterState state;
  state.FailDeleteForConnName("conn-pending");
  auto stub = MakeStub(&state);

  {
    LinkManager mgr("IEC104", linksPath, pointTablesPath);
    mgr.setDataCenterStub(stub);

    auto linkReq = MakeClientLinkReq("conn-pending");
    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(linkReq, &info).ok());

    auto status = mgr.DeleteLink("conn-pending");
    ASSERT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  }

  {
    LinkManager mgr("IEC104", linksPath, pointTablesPath);
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
  const auto linksPath = dir.path() / "links.pb";
  const auto pointTablesPath = dir.path() / "point_tables.pb";

  FakeDataCenterState initialState;
  auto initialStub = MakeStub(&initialState);

  {
    LinkManager mgr("IEC104", linksPath, pointTablesPath);
    mgr.setDataCenterStub(initialStub);

    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(MakeClientLinkReq("conn-good"), &info).ok());
    ASSERT_TRUE(mgr.UpsertLink(MakeClientLinkReq("conn-bad"), &info).ok());

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
    LinkManager mgr("IEC104", linksPath, pointTablesPath);
    mgr.setDataCenterStub(recoveredStub);

    IEC104Proto::LinkInfo good;
    ASSERT_TRUE(mgr.GetLink("conn-good", &good).ok());
    EXPECT_EQ(good.state(), IEC104Proto::LINK_STATE_STOPPED);

    IEC104Proto::PointTable pointTable;
    ASSERT_TRUE(mgr.GetPointTable("conn-good", &pointTable).ok());
    ASSERT_EQ(pointTable.points_size(), 1);
    EXPECT_EQ(pointTable.points(0).tag(), "telemetry_good");

    IEC104Proto::LinkInfo bad;
    auto status = mgr.GetLink("conn-bad", &bad);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
  }
}

// 验证：当 DataCenter 重新分配 conn_id 时，恢复后的链路会改用新 conn_id，并回写链路持久化配置。
TEST(IEC104PersistenceTest, ReloadsWithReassignedConnIdFromDataCenter) {
  ScopedTempDir dir;
  const auto linksPath = dir.path() / "links.pb";
  const auto pointTablesPath = dir.path() / "point_tables.pb";

  FakeDataCenterState initialState;
  auto initialStub = MakeStub(&initialState);

  uint32_t oldConnId = 0;
  {
    LinkManager mgr("IEC104", linksPath, pointTablesPath);
    mgr.setDataCenterStub(initialStub);

    auto linkReq = MakeClientLinkReq("conn-reassigned");
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
    LinkManager mgr("IEC104", linksPath, pointTablesPath);
    mgr.setDataCenterStub(recoveredStub);

    IEC104Proto::LinkInfo info;
    ASSERT_TRUE(mgr.GetLink("conn-reassigned", &info).ok());
    newConnId = info.conn_id();
    EXPECT_NE(newConnId, oldConnId);
    EXPECT_EQ(newConnId, oldConnId + 10);
    ASSERT_FALSE(syncedConnIds.empty());
    EXPECT_EQ(syncedConnIds.front(), newConnId);

    IEC104Proto::UpsertPointTableRequest pointReq;
    pointReq.set_conn_name("conn-reassigned");
    pointReq.set_replace(false);
    *pointReq.add_points() = MakePoint("telemetry_b", 101);
    ASSERT_TRUE(mgr.UpsertPointTable(pointReq).ok());
    ASSERT_GE(syncedConnIds.size(), 2u);
    EXPECT_EQ(syncedConnIds.back(), newConnId);
  }

  IEC104Proto::LinksConfig persisted;
  ASSERT_TRUE(LoadLinksConfigFromFile(linksPath, &persisted));
  ASSERT_EQ(persisted.links_size(), 1);
  EXPECT_EQ(persisted.links(0).config().conn_name(), "conn-reassigned");
  EXPECT_EQ(persisted.links(0).conn_id(), newConnId);
}
