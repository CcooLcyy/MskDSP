#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <tuple>

#include "DataCenterStateStore.h"

namespace {
using DataCenter::DataCenterStateStore;

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    path_ = base / ("data_center_state_store_test_tmp_" + std::to_string(ts));
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

using ConnKey = std::tuple<uint32_t, std::string, std::string>;
using TagKey = std::tuple<std::string, std::string, std::string>;
using RouteKey = std::tuple<std::string, std::string, std::string, std::string, std::string, std::string>;

std::set<ConnKey> ConnectionsToSet(const DataCenterProto::DataCenterState& state) {
  std::set<ConnKey> out;
  for (const auto& conn : state.connections().conns()) {
    out.emplace(conn.conn_id(), conn.module_name(), conn.conn_name());
  }
  return out;
}

std::set<TagKey> ConnTagsToSet(const DataCenterProto::DataCenterState& state) {
  std::set<TagKey> out;
  for (const auto& table : state.conn_tags().conn_tags()) {
    for (const auto& tag : table.tags()) {
      out.emplace(table.module_name(), table.conn_name(), tag);
    }
  }
  return out;
}

std::set<RouteKey> RoutesToSet(const DataCenterProto::DataCenterState& state) {
  std::set<RouteKey> out;
  for (const auto& route : state.routes().routes()) {
    out.emplace(route.src().module_name(), route.src().conn_name(), route.src().tag(),
                route.dst().module_name(), route.dst().conn_name(), route.dst().tag());
  }
  return out;
}

DataCenterProto::DataCenterState MakeState() {
  DataCenterProto::DataCenterState state;
  state.mutable_connections()->set_next_conn_id(4);
  auto* c1 = state.mutable_connections()->add_conns();
  c1->set_conn_id(1);
  c1->set_module_name("IEC104");
  c1->set_conn_name("IEC104");
  auto* c2 = state.mutable_connections()->add_conns();
  c2->set_conn_id(3);
  c2->set_module_name("AGC");
  c2->set_conn_name("控制组2");

  auto* tags = state.mutable_conn_tags()->add_conn_tags();
  tags->set_conn_id(3);
  tags->set_module_name("AGC");
  tags->set_conn_name("控制组2");
  tags->add_tags("AGC总有功测量点");

  auto* route = state.mutable_routes()->add_routes();
  route->mutable_src()->set_module_name("AGC");
  route->mutable_src()->set_conn_name("控制组2");
  route->mutable_src()->set_tag("AGC总有功测量点");
  route->mutable_dst()->set_module_name("IEC104");
  route->mutable_dst()->set_conn_name("IEC104");
  route->mutable_dst()->set_tag("AGC_控制组1_AGC总有功测量点");
  return state;
}

void WriteState(const std::filesystem::path& path, const DataCenterProto::DataCenterState& state) {
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(ofs.is_open());
  ASSERT_TRUE(state.SerializeToOstream(&ofs));
}
}  // 命名空间结束

// 验证：完整状态文件不存在时，Load 返回空状态且不报错。
TEST(DataCenterStateStoreTest, LoadReturnsEmptyWhenNoFiles) {
  ScopedTempDir dir;
  DataCenterStateStore store(dir.path() / "state.pb");

  DataCenterProto::DataCenterState state;
  ASSERT_TRUE(store.Load(&state).ok());
  EXPECT_EQ(state.connections().conns_size(), 0);
  EXPECT_EQ(state.conn_tags().conn_tags_size(), 0);
  EXPECT_EQ(state.routes().routes_size(), 0);
}

// 验证：完整状态 Save 后可被 Load 读取，且 connections/conn_tags/routes 属于同一份快照。
TEST(DataCenterStateStoreTest, SaveAndLoadRoundtrip) {
  ScopedTempDir dir;
  DataCenterStateStore store(dir.path() / "state.pb");

  const auto state = MakeState();
  ASSERT_TRUE(store.Save(state).ok());

  DataCenterProto::DataCenterState loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(loaded.schema_version(), 1u);
  EXPECT_EQ(loaded.connections().next_conn_id(), state.connections().next_conn_id());
  EXPECT_EQ(ConnectionsToSet(loaded), ConnectionsToSet(state));
  EXPECT_EQ(ConnTagsToSet(loaded), ConnTagsToSet(state));
  EXPECT_EQ(RoutesToSet(loaded), RoutesToSet(state));
}

// 验证：完整状态拒绝不带稳定连接主键的连接标签注册表，避免 conn_id 成为持久化主键。
TEST(DataCenterStateStoreTest, SaveRejectsConnTagsWithoutStableConnectionKey) {
  ScopedTempDir dir;
  DataCenterStateStore store(dir.path() / "state.pb");
  auto state = MakeState();
  state.mutable_conn_tags()->mutable_conn_tags(0)->clear_module_name();

  auto status = store.Save(state);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：完整状态拒绝引用不存在连接的连接标签注册表。
TEST(DataCenterStateStoreTest, SaveRejectsConnTagsReferencingMissingConnection) {
  ScopedTempDir dir;
  DataCenterStateStore store(dir.path() / "state.pb");
  auto state = MakeState();
  state.mutable_conn_tags()->mutable_conn_tags(0)->set_conn_name("不存在的连接");

  auto status = store.Save(state);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：完整状态拒绝引用不存在连接的路由，避免连接表与路由表不属于同一快照。
TEST(DataCenterStateStoreTest, SaveRejectsRoutesReferencingMissingConnection) {
  ScopedTempDir dir;
  DataCenterStateStore store(dir.path() / "state.pb");
  auto state = MakeState();
  state.mutable_routes()->mutable_routes(0)->mutable_src()->set_conn_name("不存在的连接");

  auto status = store.Save(state);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：完整状态在 conn_tags 存在时拒绝 tag 对不齐的路由。
TEST(DataCenterStateStoreTest, SaveRejectsRoutesReferencingUnregisteredTag) {
  ScopedTempDir dir;
  DataCenterStateStore store(dir.path() / "state.pb");
  auto state = MakeState();
  state.mutable_routes()->mutable_routes(0)->mutable_src()->set_tag("未注册点");

  auto status = store.Save(state);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：0 字节主状态文件不会被当作合法空状态，备份非空时会回退到备份。
TEST(DataCenterStateStoreTest, LoadFallsBackToBackupWhenMainIsZeroBytesAndBackupIsNonEmpty) {
  ScopedTempDir dir;
  const auto base = dir.path() / "state.pb";
  DataCenterStateStore store(base);

  auto state1 = MakeState();
  auto state2 = MakeState();
  state2.mutable_connections()->set_next_conn_id(8);
  state2.mutable_conn_tags()->mutable_conn_tags(0)->add_tags("新增点");

  ASSERT_TRUE(store.Save(state1).ok());
  ASSERT_TRUE(store.Save(state2).ok());

  {
    std::ofstream ofs(base, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
  }

  DataCenterProto::DataCenterState loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(loaded.schema_version(), 1u);
  EXPECT_EQ(loaded.connections().next_conn_id(), state1.connections().next_conn_id());
  EXPECT_EQ(ConnTagsToSet(loaded), ConnTagsToSet(state1));
}

// 验证：主状态文件为 0 字节且临时文件有效时，Load 优先用临时文件恢复最新完整状态。
TEST(DataCenterStateStoreTest, LoadRecoversFromValidTmpBeforeBackupWhenMainIsZeroBytes) {
  ScopedTempDir dir;
  const auto base = dir.path() / "state.pb";
  DataCenterStateStore store(base);

  auto state1 = MakeState();
  auto state2 = MakeState();
  state2.mutable_connections()->set_next_conn_id(8);
  state2.mutable_conn_tags()->mutable_conn_tags(0)->add_tags("中间点");
  auto state3 = MakeState();
  state3.set_schema_version(1);
  state3.mutable_connections()->set_next_conn_id(9);
  state3.mutable_conn_tags()->mutable_conn_tags(0)->add_tags("最新点");

  ASSERT_TRUE(store.Save(state1).ok());
  ASSERT_TRUE(store.Save(state2).ok());
  WriteState(store.tmpPath(), state3);

  {
    std::ofstream ofs(base, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
  }

  DataCenterProto::DataCenterState loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(loaded.schema_version(), 1u);
  EXPECT_EQ(loaded.connections().next_conn_id(), state3.connections().next_conn_id());
  EXPECT_EQ(ConnTagsToSet(loaded), ConnTagsToSet(state3));
  EXPECT_FALSE(std::filesystem::exists(store.tmpPath()));

  DataCenterProto::DataCenterState restoredMain;
  ASSERT_TRUE(restoredMain.ParseFromString([&]() {
    std::ifstream ifs(base, std::ios::binary);
    EXPECT_TRUE(ifs.is_open());
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }()));
  EXPECT_EQ(ConnTagsToSet(restoredMain), ConnTagsToSet(state3));
}

// 验证：主状态文件有效但临时文件更新时，Load 使用临时文件恢复最新完整状态。
TEST(DataCenterStateStoreTest, LoadRecoversFromNewerValidTmpEvenWhenMainIsValid) {
  ScopedTempDir dir;
  const auto base = dir.path() / "state.pb";
  DataCenterStateStore store(base);

  auto state1 = MakeState();
  auto state2 = MakeState();
  state2.set_schema_version(1);
  state2.mutable_connections()->set_next_conn_id(10);
  state2.mutable_conn_tags()->mutable_conn_tags(0)->add_tags("最新点");

  ASSERT_TRUE(store.Save(state1).ok());
  WriteState(store.tmpPath(), state2);
  const auto oldTime = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(2);
  const auto newTime = oldTime + std::chrono::seconds(1);
  std::filesystem::last_write_time(base, oldTime);
  std::filesystem::last_write_time(store.tmpPath(), newTime);

  DataCenterProto::DataCenterState loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(loaded.schema_version(), 1u);
  EXPECT_EQ(loaded.connections().next_conn_id(), state2.connections().next_conn_id());
  EXPECT_EQ(ConnTagsToSet(loaded), ConnTagsToSet(state2));
  EXPECT_FALSE(std::filesystem::exists(store.tmpPath()));

  DataCenterProto::DataCenterState backup;
  ASSERT_TRUE(backup.ParseFromString([&]() {
    std::ifstream ifs(store.backupPath(), std::ios::binary);
    EXPECT_TRUE(ifs.is_open());
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }()));
  EXPECT_EQ(ConnTagsToSet(backup), ConnTagsToSet(state1));
}

// 验证：主状态文件损坏时 Load 会回退到备份文件，并 best-effort 恢复主文件。
TEST(DataCenterStateStoreTest, LoadFallsBackToBackupWhenMainCorruptedAndRestoresMainBestEffort) {
  ScopedTempDir dir;
  const auto base = dir.path() / "state.pb";
  DataCenterStateStore store(base);

  auto state1 = MakeState();
  auto state2 = MakeState();
  state2.mutable_connections()->set_next_conn_id(8);
  state2.mutable_conn_tags()->mutable_conn_tags(0)->add_tags("新增点");

  ASSERT_TRUE(store.Save(state1).ok());
  ASSERT_TRUE(store.Save(state2).ok());

  {
    std::ofstream ofs(base, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << "corrupt";
  }

  DataCenterProto::DataCenterState loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(loaded.connections().next_conn_id(), state1.connections().next_conn_id());
  EXPECT_EQ(ConnTagsToSet(loaded), ConnTagsToSet(state1));

  DataCenterProto::DataCenterState restoredMain;
  ASSERT_TRUE(restoredMain.ParseFromString([&]() {
    std::ifstream ifs(base, std::ios::binary);
    EXPECT_TRUE(ifs.is_open());
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }()));
  EXPECT_EQ(ConnTagsToSet(restoredMain), ConnTagsToSet(state1));
}

// 验证：backupPath/tmpPath 派生规则与构造路径一致。
TEST(DataCenterStateStoreTest, BackupAndTmpPathsAreDerivedFromBasePath) {
  ScopedTempDir dir;
  const auto base = dir.path() / "state.pb";
  DataCenterStateStore store(base);

  EXPECT_EQ(store.statePath(), base);
  EXPECT_EQ(store.backupPath(), std::filesystem::path(base.string() + ".bak"));
  EXPECT_EQ(store.tmpPath(), std::filesystem::path(base.string() + ".tmp"));
}
