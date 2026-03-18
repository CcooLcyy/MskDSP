#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include "AGCGroupStore.h"

namespace {
using AGC::AGCGroupStore;

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    path_ = base / ("agc_group_store_test_tmp_" + std::to_string(ts));
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

AGCProto::GroupConfig MakeGroupConfig(const char* groupName) {
  AGCProto::GroupConfig cfg;
  cfg.set_group_name(groupName);
  cfg.mutable_p_cmd()->mutable_signal()->set_tag("P_CMD");
  cfg.mutable_p_cmd()->mutable_signal()->set_unit("kW");
  cfg.mutable_p_cmd()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);

  auto* member = cfg.add_members();
  member->set_member_name("inv-1");
  member->set_controllable(true);
  member->set_capacity_kw(50.0);
  member->set_weight(50.0);
  member->mutable_p_meas()->set_tag("INV1_P_MEAS");
  member->mutable_p_meas()->set_unit("kW");
  member->mutable_p_set()->mutable_signal()->set_tag("INV1_P_SET");
  member->mutable_p_set()->mutable_signal()->set_unit("kW");
  member->mutable_p_set()->set_mode(AGCProto::VALUE_MODE_ABSOLUTE);
  return cfg;
}

AGCProto::GroupsConfig MakeGroupsConfig() {
  AGCProto::GroupsConfig cfg;
  auto* persisted1 = cfg.add_persisted_groups();
  *persisted1->mutable_config() = MakeGroupConfig("g-1");
  auto* persisted2 = cfg.add_persisted_groups();
  *persisted2->mutable_config() = MakeGroupConfig("g-2");
  persisted2->set_pending_delete(true);
  return cfg;
}

std::set<std::string> ToGroupNameSet(const AGCProto::GroupsConfig& config) {
  std::set<std::string> names;
  if (config.persisted_groups_size() > 0) {
    for (const auto& group : config.persisted_groups()) {
      if (group.has_config()) {
        names.emplace(group.config().group_name());
      }
    }
    return names;
  }
  for (const auto& group : config.groups()) {
    names.emplace(group.group_name());
  }
  return names;
}

bool IsPendingDelete(const AGCProto::GroupsConfig& config, const std::string& groupName) {
  for (const auto& group : config.persisted_groups()) {
    if (group.has_config() && group.config().group_name() == groupName) {
      return group.pending_delete();
    }
  }
  return false;
}

}  // 命名空间结束

// 验证：当控制组配置文件不存在时，Load 返回空配置且不报错。
TEST(AgcGroupStoreTest, LoadReturnsEmptyWhenNoFiles) {
  ScopedTempDir dir;
  AGCGroupStore store(dir.path() / "groups.pb");

  AGCProto::GroupsConfig cfg;
  ASSERT_TRUE(store.Load(&cfg).ok());
  EXPECT_EQ(cfg.groups_size(), 0);
}

// 验证：Save 后可被 Load 读取，且控制组名集合保持一致。
TEST(AgcGroupStoreTest, SaveAndLoadRoundtrip) {
  ScopedTempDir dir;
  AGCGroupStore store(dir.path() / "groups.pb");

  auto cfg = MakeGroupsConfig();
  ASSERT_TRUE(store.Save(cfg).ok());

  AGCProto::GroupsConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(ToGroupNameSet(loaded), ToGroupNameSet(cfg));
  EXPECT_TRUE(IsPendingDelete(loaded, "g-2"));
}

// 验证：Save 会拒绝非法配置（例如重复的 group_name）。
TEST(AgcGroupStoreTest, SaveRejectsInvalidConfig) {
  ScopedTempDir dir;
  AGCGroupStore store(dir.path() / "groups.pb");

  AGCProto::GroupsConfig cfg;
  auto* persisted1 = cfg.add_persisted_groups();
  *persisted1->mutable_config() = MakeGroupConfig("dup");
  auto* persisted2 = cfg.add_persisted_groups();
  *persisted2->mutable_config() = MakeGroupConfig("dup");

  auto status = store.Save(cfg);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：Load 兼容旧版仅包含 groups 字段的落盘格式。
TEST(AgcGroupStoreTest, LoadSupportsLegacyGroupsField) {
  ScopedTempDir dir;
  AGCGroupStore store(dir.path() / "groups.pb");

  AGCProto::GroupsConfig legacy;
  *legacy.add_groups() = MakeGroupConfig("legacy-g-1");
  *legacy.add_groups() = MakeGroupConfig("legacy-g-2");
  ASSERT_TRUE(store.Save(legacy).ok());

  AGCProto::GroupsConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(ToGroupNameSet(loaded), ToGroupNameSet(legacy));
}

// 验证：主文件损坏时，Load 会回退到备份文件并 best-effort 恢复主文件。
TEST(AgcGroupStoreTest, LoadFallsBackToBackupWhenMainCorrupted) {
  ScopedTempDir dir;
  const auto base = dir.path() / "groups.pb";
  AGCGroupStore store(base);

  AGCProto::GroupsConfig cfg1;
  *cfg1.add_groups() = MakeGroupConfig("g-1");
  AGCProto::GroupsConfig cfg2;
  *cfg2.add_groups() = MakeGroupConfig("g-1");
  *cfg2.add_groups() = MakeGroupConfig("g-2");

  ASSERT_TRUE(store.Save(cfg1).ok());
  ASSERT_TRUE(store.Save(cfg2).ok());

  {
    std::ofstream ofs(base, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << "corrupt";
  }

  AGCProto::GroupsConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(ToGroupNameSet(loaded), ToGroupNameSet(cfg1));

  AGCProto::GroupsConfig restoredMain;
  ASSERT_TRUE(restoredMain.ParseFromString([&]() {
    std::ifstream ifs(base, std::ios::binary);
    EXPECT_TRUE(ifs.is_open());
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }()));
  EXPECT_EQ(ToGroupNameSet(restoredMain), ToGroupNameSet(cfg1));
}

// 验证：backupPath/tmpPath 派生规则与构造路径一致。
TEST(AgcGroupStoreTest, BackupAndTmpPathsAreDerivedFromBasePath) {
  ScopedTempDir dir;
  const auto base = dir.path() / "groups.pb";
  AGCGroupStore store(base);

  EXPECT_EQ(store.groupsPath(), base);
  EXPECT_EQ(store.backupPath(), std::filesystem::path(base.string() + ".bak"));
  EXPECT_EQ(store.tmpPath(), std::filesystem::path(base.string() + ".tmp"));
}
