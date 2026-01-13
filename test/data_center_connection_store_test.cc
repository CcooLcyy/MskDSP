#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <tuple>

#include "DataCenterConnectionStore.h"

namespace {
using DataCenter::DataCenterConnectionStore;

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    path_ = base / ("data_center_connection_store_test_tmp_" + std::to_string(ts));
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

std::set<ConnKey> ToSet(const DataCenterProto::ConnectionsConfig& cfg) {
  std::set<ConnKey> out;
  for (const auto& conn : cfg.conns()) {
    out.emplace(conn.conn_id(), conn.module_name(), conn.conn_name());
  }
  return out;
}

DataCenterProto::ConnectionsConfig MakeConfig(uint32_t nextConnId, std::initializer_list<std::tuple<uint32_t, const char*, const char*>> conns) {
  DataCenterProto::ConnectionsConfig cfg;
  cfg.set_next_conn_id(nextConnId);
  for (const auto& [id, moduleName, connName] : conns) {
    auto* conn = cfg.add_conns();
    conn->set_conn_id(id);
    conn->set_module_name(moduleName);
    conn->set_conn_name(connName);
  }
  return cfg;
}
}  // namespace

TEST(DataCenterConnectionStoreTest, LoadReturnsEmptyWhenNoFiles) {
  ScopedTempDir dir;
  DataCenterConnectionStore store(dir.path() / "connections.pb");

  DataCenterProto::ConnectionsConfig cfg;
  ASSERT_TRUE(store.Load(&cfg).ok());
  EXPECT_EQ(cfg.conns_size(), 0);
}

TEST(DataCenterConnectionStoreTest, SaveAndLoadRoundtrip) {
  ScopedTempDir dir;
  DataCenterConnectionStore store(dir.path() / "connections.pb");

  auto cfg = MakeConfig(100, {
                              {1, "IEC104", "104-1"},
                              {2, "Modbus", "mb-1"},
                          });
  ASSERT_TRUE(store.Save(cfg).ok());

  DataCenterProto::ConnectionsConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(loaded.next_conn_id(), 100u);
  EXPECT_EQ(ToSet(loaded), ToSet(cfg));
}

TEST(DataCenterConnectionStoreTest, SaveRejectsInvalidConfig) {
  ScopedTempDir dir;
  DataCenterConnectionStore store(dir.path() / "connections.pb");

  DataCenterProto::ConnectionsConfig cfg;
  auto* conn = cfg.add_conns();
  conn->set_conn_id(0);
  conn->set_module_name("x");
  conn->set_conn_name("y");

  auto status = store.Save(cfg);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(DataCenterConnectionStoreTest, LoadFallsBackToBackupWhenMainCorruptedAndRestoresMainBestEffort) {
  ScopedTempDir dir;
  const auto base = dir.path() / "connections.pb";
  DataCenterConnectionStore store(base);

  auto cfg1 = MakeConfig(10, {
                                 {1, "IEC104", "104-1"},
                             });
  auto cfg2 = MakeConfig(11, {
                                 {1, "IEC104", "104-1"},
                                 {2, "Modbus", "mb-1"},
                             });

  ASSERT_TRUE(store.Save(cfg1).ok());
  ASSERT_TRUE(store.Save(cfg2).ok());

  {
    std::ofstream ofs(base, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << "corrupt";
  }

  DataCenterProto::ConnectionsConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(ToSet(loaded), ToSet(cfg1));
  EXPECT_EQ(loaded.next_conn_id(), cfg1.next_conn_id());

  DataCenterProto::ConnectionsConfig restoredMain;
  ASSERT_TRUE(restoredMain.ParseFromString([&]() {
    std::ifstream ifs(base, std::ios::binary);
    EXPECT_TRUE(ifs.is_open());
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }()));
  EXPECT_EQ(ToSet(restoredMain), ToSet(cfg1));

  bool foundCorrupt = false;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
    const auto name = entry.path().filename().string();
    if (name.rfind("connections.pb.corrupt.", 0) == 0) {
      foundCorrupt = true;
      break;
    }
  }
  EXPECT_TRUE(foundCorrupt);
}

