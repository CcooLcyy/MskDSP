#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>

#include "DataCenterPointTableStore.h"

namespace {
using DataCenter::DataCenterPointTableStore;

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    path_ = base / ("data_center_store_test_tmp_" + std::to_string(ts));
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

std::unordered_map<uint32_t, std::set<std::string>> ToMap(const DataCenterProto::PointTablesConfig& cfg) {
  std::unordered_map<uint32_t, std::set<std::string>> out;
  for (const auto& table : cfg.point_tables()) {
    auto& set = out[table.conn_id()];
    for (const auto& tag : table.tags()) {
      set.emplace(tag);
    }
  }
  return out;
}

DataCenterProto::PointTablesConfig MakeConfig(std::initializer_list<std::pair<uint32_t, std::initializer_list<const char*>>> tables) {
  DataCenterProto::PointTablesConfig cfg;
  for (const auto& [connId, tags] : tables) {
    auto* table = cfg.add_point_tables();
    table->set_conn_id(connId);
    for (const auto* tag : tags) {
      table->add_tags(tag);
    }
  }
  return cfg;
}
}  // namespace

// 验证：当点表配置文件不存在时，Load 返回空配置且不报错。
TEST(DataCenterPointTableStoreTest, LoadReturnsEmptyWhenNoFiles) {
  ScopedTempDir dir;
  DataCenterPointTableStore store(dir.path() / "point_tables.pb");

  DataCenterProto::PointTablesConfig cfg;
  ASSERT_TRUE(store.Load(&cfg).ok());
  EXPECT_EQ(cfg.point_tables_size(), 0);
}

// 验证：Save 后可被 Load 读取，且内容一致（roundtrip）。
TEST(DataCenterPointTableStoreTest, SaveAndLoadRoundtrip) {
  ScopedTempDir dir;
  DataCenterPointTableStore store(dir.path() / "point_tables.pb");

  auto cfg = MakeConfig({
      {1, {"点1", "点2"}},
      {2, {"A", "B"}},
  });
  ASSERT_TRUE(store.Save(cfg).ok());

  DataCenterProto::PointTablesConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(ToMap(loaded), ToMap(cfg));
}

// 验证：Save 会拒绝非法配置（例如 conn_id=0 或空 tag）。
TEST(DataCenterPointTableStoreTest, SaveRejectsInvalidConfig) {
  ScopedTempDir dir;
  DataCenterPointTableStore store(dir.path() / "point_tables.pb");

  DataCenterProto::PointTablesConfig cfg;
  auto* table = cfg.add_point_tables();
  table->set_conn_id(0);
  table->add_tags("x");

  auto status = store.Save(cfg);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：主文件损坏时 Load 会回退到备份文件，并 best-effort 恢复主文件。
TEST(DataCenterPointTableStoreTest, LoadFallsBackToBackupWhenMainCorruptedAndRestoresMainBestEffort) {
  ScopedTempDir dir;
  const auto base = dir.path() / "point_tables.pb";
  DataCenterPointTableStore store(base);

  auto cfg1 = MakeConfig({
      {1, {"源点"}},
  });
  auto cfg2 = MakeConfig({
      {1, {"源点", "新增点"}},
  });

  ASSERT_TRUE(store.Save(cfg1).ok());
  ASSERT_TRUE(store.Save(cfg2).ok());

  {
    std::ofstream ofs(base, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << "corrupt";
  }

  DataCenterProto::PointTablesConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(ToMap(loaded), ToMap(cfg1));

  DataCenterProto::PointTablesConfig restoredMain;
  ASSERT_TRUE(restoredMain.ParseFromString([&]() {
    std::ifstream ifs(base, std::ios::binary);
    EXPECT_TRUE(ifs.is_open());
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }()));
  EXPECT_EQ(ToMap(restoredMain), ToMap(cfg1));

  bool foundCorrupt = false;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
    const auto name = entry.path().filename().string();
    if (name.rfind("point_tables.pb.corrupt.", 0) == 0) {
      foundCorrupt = true;
      break;
    }
  }
  EXPECT_TRUE(foundCorrupt);
}
