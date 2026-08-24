#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "AGCControlProfileStore.h"

namespace {
using AGC::AGCControlProfileStore;

class ScopedTempDir {
public:
  ScopedTempDir() {
    const auto base = std::filesystem::current_path();
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    path_ = base / ("agc_control_profile_store_test_tmp_" + std::to_string(timestamp));
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

AGCProto::ControlProfilesConfig MakeProfilesConfig() {
  AGCProto::ControlProfilesConfig config;
  auto* group = config.add_profiles();
  group->set_group_name("group-1");
  group->set_version(12);
  group->set_confirmed_at_ms(1700000000123ULL);

  auto* member = group->add_members();
  member->set_member_name("inverter-1");
  member->set_up_p_gain(0.75);
  member->set_up_i_gain(0.025);
  member->set_down_p_gain(0.60);
  member->set_down_i_gain(0.015);
  member->set_up_bias_kw(1.5);
  member->set_down_bias_kw(-0.8);
  member->set_integral_limit_kw(20.0);
  member->set_max_step_kw(8.0);
  member->set_max_ramp_kw_per_s(2.0);
  member->set_version(12);
  member->set_confirmed_at_ms(1700000000123ULL);
  return config;
}

}  // namespace

// 验证：控制参数数据库不存在时，Load 返回空配置且不报错。
TEST(AgcControlProfileStoreTest, LoadReturnsEmptyWhenNoFiles) {
  ScopedTempDir dir;
  AGCControlProfileStore store(dir.path() / "config.db");

  AGCProto::ControlProfilesConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(loaded.profiles_size(), 0);
}

// 验证：控制组和成员的固定参数 Save 后可从 SQLite 完整 Load，字段保持一致。
TEST(AgcControlProfileStoreTest, SaveAndLoadRoundtrip) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "config.db";
  AGCControlProfileStore store(configDbPath);
  const auto config = MakeProfilesConfig();

  ASSERT_TRUE(store.Save(config).ok());
  EXPECT_TRUE(std::filesystem::exists(configDbPath));

  AGCProto::ControlProfilesConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(loaded.SerializeAsString(), config.SerializeAsString());
  EXPECT_EQ(store.databasePath(), configDbPath);
}

// 验证：Save 拒绝成员控制参数中的负比例或积分系数。
TEST(AgcControlProfileStoreTest, SaveRejectsNegativeGain) {
  ScopedTempDir dir;
  AGCControlProfileStore store(dir.path() / "config.db");
  auto config = MakeProfilesConfig();
  config.mutable_profiles(0)->mutable_members(0)->set_up_p_gain(-0.1);

  const auto status = store.Save(config);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：Save 拒绝同一控制组内重复的成员名称，避免参数无法唯一匹配设备。
TEST(AgcControlProfileStoreTest, SaveRejectsDuplicateMemberName) {
  ScopedTempDir dir;
  AGCControlProfileStore store(dir.path() / "config.db");
  auto config = MakeProfilesConfig();
  const auto member = config.profiles(0).members(0);
  *config.mutable_profiles(0)->add_members() = member;

  const auto status = store.Save(config);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
