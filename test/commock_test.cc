#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "COMMock.h"
#include "COMMock.pb.h"

namespace {
class TempDir {
public:
  explicit TempDir(const std::string &prefix) {
    const std::filesystem::path base("/tmp");
    for (int i = 0; i < 10; ++i) {
      const auto suffix = std::to_string(::getpid()) + "_" + std::to_string(counter_.fetch_add(1));
      path_ = base / (prefix + "_" + suffix);
      std::error_code ec;
      if (std::filesystem::create_directories(path_, ec)) {
        return;
      }
    }
  }

  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }

  const std::filesystem::path &path() const {
    return path_;
  }

private:
  std::filesystem::path path_;
  static std::atomic<int> counter_;
};

std::atomic<int> TempDir::counter_{0};
}  // namespace

// 验证 ApplyConfig 在缺少 dev_path 时返回 INVALID_ARGUMENT。
TEST(COMMockTest, RejectsMissingDevPath) {
  COMMock::COMMock module;
  COMMockProto::COMMockConfig config;
  auto *port = config.add_ports();
  port->set_name("p1");

  const auto status = module.ApplyConfig(config);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证 ApplyConfig 会创建 dev_path 软链并在再次下发时替换旧端口。
TEST(COMMockTest, ApplyConfigReplacesPorts) {
  TempDir dir("commock_test");
  ASSERT_FALSE(dir.path().empty());

  const auto path0 = dir.path() / "COMMock0";
  const auto path1 = dir.path() / "COMMock1";

  COMMock::COMMock module;
  COMMockProto::COMMockConfig config;
  auto *port = config.add_ports();
  port->set_name("p0");
  port->set_dev_path(path0.string());

  auto status = module.ApplyConfig(config);
  ASSERT_EQ(status.error_code(), grpc::StatusCode::OK);

  std::error_code ec;
  EXPECT_TRUE(std::filesystem::is_symlink(path0, ec));
  EXPECT_FALSE(ec);

  COMMockProto::COMMockConfig config2;
  auto *port2 = config2.add_ports();
  port2->set_name("p1");
  port2->set_dev_path(path1.string());

  status = module.ApplyConfig(config2);
  ASSERT_EQ(status.error_code(), grpc::StatusCode::OK);

  ec.clear();
  EXPECT_FALSE(std::filesystem::exists(path0, ec));
  EXPECT_FALSE(ec);
  ec.clear();
  EXPECT_TRUE(std::filesystem::is_symlink(path1, ec));
  EXPECT_FALSE(ec);
}

// 验证 ApplyConfig 空配置会清理已创建端口。
TEST(COMMockTest, ApplyConfigEmptyClearsPorts) {
  TempDir dir("commock_test");
  ASSERT_FALSE(dir.path().empty());

  const auto path0 = dir.path() / "COMMock0";

  COMMock::COMMock module;
  COMMockProto::COMMockConfig config;
  auto *port = config.add_ports();
  port->set_dev_path(path0.string());

  auto status = module.ApplyConfig(config);
  ASSERT_EQ(status.error_code(), grpc::StatusCode::OK);

  std::error_code ec;
  ASSERT_TRUE(std::filesystem::is_symlink(path0, ec));
  ASSERT_FALSE(ec);

  COMMockProto::COMMockConfig empty;
  status = module.ApplyConfig(empty);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::OK);

  ec.clear();
  EXPECT_FALSE(std::filesystem::exists(path0, ec));
  EXPECT_FALSE(ec);
}
