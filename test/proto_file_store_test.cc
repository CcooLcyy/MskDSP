#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include "DataCenter.pb.h"
#include "mskdsp/detail/ProtoFileStore.hpp"

namespace {
using ConnectionsStore = mskdsp::detail::ProtoFileStore<DataCenterProto::ConnectionsConfig>;

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    path_ = base / ("proto_file_store_test_tmp_" + std::to_string(ts));
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

DataCenterProto::ConnectionsConfig MakeConfig(
    uint32_t nextConnId,
    std::initializer_list<std::tuple<uint32_t, const char*, const char*>> conns) {
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

grpc::Status ValidateConnectionsConfig(const DataCenterProto::ConnectionsConfig& config) {
  std::set<uint32_t> ids;
  std::set<std::pair<std::string, std::string>> keys;

  for (const auto& conn : config.conns()) {
    if (conn.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含 conn_id=0");
    }
    if (conn.module_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含空 module_name");
    }
    if (conn.conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含空 conn_name");
    }

    if (!ids.emplace(conn.conn_id()).second) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含重复的 conn_id");
    }
    if (!keys.emplace(conn.module_name(), conn.conn_name()).second) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conns 包含重复的 (module_name, conn_name)");
    }
  }

  return grpc::Status::OK;
}

void WriteConfig(const std::filesystem::path& path, const DataCenterProto::ConnectionsConfig& cfg) {
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(ofs.is_open());
  ASSERT_TRUE(cfg.SerializeToOstream(&ofs));
}
}  // namespace

// 验证：当持久化文件不存在时，Load 返回空配置且不报错。
TEST(ProtoFileStoreTest, LoadReturnsEmptyWhenNoFiles) {
  ScopedTempDir dir;
  ConnectionsStore store(dir.path() / "connections.pb", ValidateConnectionsConfig);

  DataCenterProto::ConnectionsConfig cfg;
  ASSERT_TRUE(store.Load(&cfg).ok());
  EXPECT_EQ(cfg.conns_size(), 0);
}

// 验证：Save 后可被 Load 读取，且内容一致。
TEST(ProtoFileStoreTest, SaveAndLoadRoundtrip) {
  ScopedTempDir dir;
  ConnectionsStore store(dir.path() / "connections.pb", ValidateConnectionsConfig);

  auto cfg = MakeConfig(10, {
                                {1, "IEC104", "104-1"},
                                {2, "ModbusRTU", "mb-1"},
                            });
  ASSERT_TRUE(store.Save(cfg).ok());

  DataCenterProto::ConnectionsConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(loaded.next_conn_id(), 10u);
  EXPECT_EQ(ToSet(loaded), ToSet(cfg));
}

// 验证：Save 会拒绝非法配置。
TEST(ProtoFileStoreTest, SaveRejectsInvalidConfig) {
  ScopedTempDir dir;
  ConnectionsStore store(dir.path() / "connections.pb", ValidateConnectionsConfig);

  auto cfg = MakeConfig(10, {
                                {0, "IEC104", "104-1"},
                            });

  auto status = store.Save(cfg);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：主文件损坏时，Load 会回退到备份文件并 best-effort 恢复主文件。
TEST(ProtoFileStoreTest, LoadFallsBackToBackupWhenMainCorruptedAndRestoresMainBestEffort) {
  ScopedTempDir dir;
  const auto base = dir.path() / "connections.pb";
  ConnectionsStore store(base, ValidateConnectionsConfig);

  auto cfg1 = MakeConfig(10, {
                                 {1, "IEC104", "104-1"},
                             });
  auto cfg2 = MakeConfig(11, {
                                 {1, "IEC104", "104-1"},
                                 {2, "ModbusRTU", "mb-1"},
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
  EXPECT_EQ(loaded.next_conn_id(), cfg1.next_conn_id());
  EXPECT_EQ(ToSet(loaded), ToSet(cfg1));

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

// 验证：主文件损坏且临时文件有效时，Load 优先使用临时文件恢复最新配置，避免回退到落后一代备份。
TEST(ProtoFileStoreTest, LoadRecoversFromValidTmpBeforeBackupWhenMainCorrupted) {
  ScopedTempDir dir;
  const auto base = dir.path() / "connections.pb";
  ConnectionsStore store(base, ValidateConnectionsConfig);

  auto cfg1 = MakeConfig(10, {
                                 {1, "IEC104", "104-1"},
                             });
  auto cfg2 = MakeConfig(11, {
                                 {1, "IEC104", "104-1"},
                                 {2, "ModbusRTU", "mb-1"},
                             });
  auto cfg3 = MakeConfig(12, {
                                 {1, "IEC104", "104-1"},
                                 {2, "ModbusRTU", "mb-1"},
                                 {3, "DLT645", "dlt-1"},
                             });

  ASSERT_TRUE(store.Save(cfg1).ok());
  ASSERT_TRUE(store.Save(cfg2).ok());
  WriteConfig(store.tmpPath(), cfg3);

  {
    std::ofstream ofs(base, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << "corrupt";
  }

  DataCenterProto::ConnectionsConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(loaded.next_conn_id(), cfg3.next_conn_id());
  EXPECT_EQ(ToSet(loaded), ToSet(cfg3));
  EXPECT_FALSE(std::filesystem::exists(store.tmpPath()));

  DataCenterProto::ConnectionsConfig restoredMain;
  ASSERT_TRUE(restoredMain.ParseFromString([&]() {
    std::ifstream ifs(base, std::ios::binary);
    EXPECT_TRUE(ifs.is_open());
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }()));
  EXPECT_EQ(ToSet(restoredMain), ToSet(cfg3));
}

// 验证：主文件有效但临时文件更新时，Load 使用临时文件完成恢复，避免丢失已写入 tmp 的最新一代配置。
TEST(ProtoFileStoreTest, LoadRecoversFromNewerValidTmpEvenWhenMainIsValid) {
  ScopedTempDir dir;
  const auto base = dir.path() / "connections.pb";
  ConnectionsStore store(base, ValidateConnectionsConfig);

  auto cfg1 = MakeConfig(10, {
                                 {1, "IEC104", "104-1"},
                             });
  auto cfg2 = MakeConfig(11, {
                                 {1, "IEC104", "104-1"},
                                 {2, "ModbusRTU", "mb-1"},
                             });

  ASSERT_TRUE(store.Save(cfg1).ok());
  WriteConfig(store.tmpPath(), cfg2);
  const auto oldTime = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(2);
  const auto newTime = oldTime + std::chrono::seconds(1);
  std::filesystem::last_write_time(base, oldTime);
  std::filesystem::last_write_time(store.tmpPath(), newTime);

  DataCenterProto::ConnectionsConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(loaded.next_conn_id(), cfg2.next_conn_id());
  EXPECT_EQ(ToSet(loaded), ToSet(cfg2));
  EXPECT_FALSE(std::filesystem::exists(store.tmpPath()));

  DataCenterProto::ConnectionsConfig backup;
  ASSERT_TRUE(backup.ParseFromString([&]() {
    std::ifstream ifs(store.backupPath(), std::ios::binary);
    EXPECT_TRUE(ifs.is_open());
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }()));
  EXPECT_EQ(ToSet(backup), ToSet(cfg1));
}

// 验证：主文件和备份文件同时损坏时，Load 返回错误而不是静默返回空配置。
TEST(ProtoFileStoreTest, LoadReturnsErrorWhenMainAndBackupAreBothCorrupted) {
  ScopedTempDir dir;
  const auto base = dir.path() / "connections.pb";
  ConnectionsStore store(base, ValidateConnectionsConfig);

  {
    std::ofstream ofs(base, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << "corrupt-main";
  }
  {
    std::ofstream ofs(store.backupPath(), std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << "corrupt-backup";
  }

  DataCenterProto::ConnectionsConfig loaded;
  auto status = store.Load(&loaded);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(loaded.conns_size(), 0);
  EXPECT_FALSE(std::filesystem::exists(base));
  EXPECT_FALSE(std::filesystem::exists(store.backupPath()));

  bool foundMainCorrupt = false;
  bool foundBackupCorrupt = false;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
    const auto name = entry.path().filename().string();
    if (name.rfind("connections.pb.corrupt.", 0) == 0) {
      foundMainCorrupt = true;
    }
    if (name.rfind("connections.pb.bak.corrupt.", 0) == 0) {
      foundBackupCorrupt = true;
    }
  }
  EXPECT_TRUE(foundMainCorrupt);
  EXPECT_TRUE(foundBackupCorrupt);
}

// 验证：backupPath/tmpPath 派生规则与构造路径一致。
TEST(ProtoFileStoreTest, BackupAndTmpPathsAreDerivedFromBasePath) {
  ScopedTempDir dir;
  const auto base = dir.path() / "connections.pb";
  ConnectionsStore store(base, ValidateConnectionsConfig);

  EXPECT_EQ(store.path(), base);
  EXPECT_EQ(store.backupPath(), std::filesystem::path(base.string() + ".bak"));
  EXPECT_EQ(store.tmpPath(), std::filesystem::path(base.string() + ".tmp"));
}

// 验证：ValidateFn 为空时，Save 返回 INTERNAL。
TEST(ProtoFileStoreTest, SaveReturnsInternalWhenValidateIsNull) {
  ScopedTempDir dir;
  ConnectionsStore store(dir.path() / "connections.pb", nullptr);

  auto status = store.Save(MakeConfig(10, {
                                           {1, "IEC104", "104-1"},
                                       }));
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
}
