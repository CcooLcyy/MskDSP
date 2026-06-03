#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
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

// 验证：当 SQLite 控制组配置不存在时，Load 返回空配置且不报错。
TEST(AgcGroupStoreTest, LoadReturnsEmptyWhenNoFiles) {
  ScopedTempDir dir;
  AGCGroupStore store(dir.path() / "config.db");

  AGCProto::GroupsConfig cfg;
  ASSERT_TRUE(store.Load(&cfg).ok());
  EXPECT_EQ(cfg.groups_size(), 0);
}

// 验证：Save 后可从 SQLite Load 读取，且控制组名集合保持一致。
TEST(AgcGroupStoreTest, SaveAndLoadRoundtrip) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";
  AGCGroupStore store(configDbPath);

  auto cfg = MakeGroupsConfig();
  ASSERT_TRUE(store.Save(cfg).ok());
  EXPECT_TRUE(std::filesystem::exists(configDbPath));

  AGCProto::GroupsConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(ToGroupNameSet(loaded), ToGroupNameSet(cfg));
  EXPECT_TRUE(IsPendingDelete(loaded, "g-2"));
}

// 验证：Save 会拒绝非法配置（例如重复的 group_name）。
TEST(AgcGroupStoreTest, SaveRejectsInvalidConfig) {
  ScopedTempDir dir;
  AGCGroupStore store(dir.path() / "config.db");

  AGCProto::GroupsConfig cfg;
  auto* persisted1 = cfg.add_persisted_groups();
  *persisted1->mutable_config() = MakeGroupConfig("dup");
  auto* persisted2 = cfg.add_persisted_groups();
  *persisted2->mutable_config() = MakeGroupConfig("dup");

  auto status = store.Save(cfg);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：Load 兼容旧版仅包含 groups 字段的 protobuf payload。
TEST(AgcGroupStoreTest, LoadSupportsLegacyGroupsField) {
  ScopedTempDir dir;
  AGCGroupStore store(dir.path() / "config.db");

  AGCProto::GroupsConfig legacy;
  *legacy.add_groups() = MakeGroupConfig("legacy-g-1");
  *legacy.add_groups() = MakeGroupConfig("legacy-g-2");
  ASSERT_TRUE(store.Save(legacy).ok());

  AGCProto::GroupsConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(ToGroupNameSet(loaded), ToGroupNameSet(legacy));
}

// 验证：databasePath 返回构造时传入的 SQLite 数据库路径。
TEST(AgcGroupStoreTest, DatabasePathReturnsConfiguredPath) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";
  AGCGroupStore store(configDbPath);

  EXPECT_EQ(store.databasePath(), configDbPath);
}
