#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
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

bool HasSocat() {
  const char *path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return false;
  }
  const std::string paths(path_env);
  size_t start = 0;
  while (start <= paths.size()) {
    size_t end = paths.find(':', start);
    if (end == std::string::npos) {
      end = paths.size();
    }
    std::string dir = paths.substr(start, end - start);
    if (dir.empty()) {
      dir = ".";
    }
    const std::filesystem::path candidate = std::filesystem::path(dir) / "socat";
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0) {
      return true;
    }
    start = end + 1;
  }
  return false;
}
}  // namespace

// 验证 ApplyConfig 在缺少 dev_path 时返回 INVALID_ARGUMENT。
TEST(COMMockTest, RejectsMissingDevPath) {
  COMMock::COMMock module;
  COMMockProto::COMMockConfig config;
  auto *port = config.add_ports();
  port->set_name("p1");
  port->set_peer_name("p2");

  const auto status = module.ApplyConfig(config);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证 ApplyConfig 会创建 dev_path 软链并在再次下发时替换旧端口。
TEST(COMMockTest, ApplyConfigReplacesPorts) {
  if (!HasSocat()) {
    GTEST_SKIP() << "socat 未安装或不在 PATH 中";
  }
  TempDir dir("commock_test");
  ASSERT_FALSE(dir.path().empty());

  const auto path0 = dir.path() / "COMMock0";
  const auto path1 = dir.path() / "COMMock1";
  const auto path2 = dir.path() / "COMMock2";
  const auto path3 = dir.path() / "COMMock3";

  COMMock::COMMock module;
  COMMockProto::COMMockConfig config;
  auto *port0 = config.add_ports();
  port0->set_name("p0");
  port0->set_dev_path(path0.string());
  port0->set_peer_name("p1");
  auto *port1 = config.add_ports();
  port1->set_name("p1");
  port1->set_dev_path(path1.string());
  port1->set_peer_name("p0");

  auto status = module.ApplyConfig(config);
  ASSERT_EQ(status.error_code(), grpc::StatusCode::OK);

  std::error_code ec;
  EXPECT_TRUE(std::filesystem::is_symlink(path0, ec));
  EXPECT_FALSE(ec);
  ec.clear();
  EXPECT_TRUE(std::filesystem::is_symlink(path1, ec));
  EXPECT_FALSE(ec);

  COMMockProto::COMMockConfig config2;
  auto *port2 = config2.add_ports();
  port2->set_name("p2");
  port2->set_dev_path(path2.string());
  port2->set_peer_name("p3");
  auto *port3 = config2.add_ports();
  port3->set_name("p3");
  port3->set_dev_path(path3.string());
  port3->set_peer_name("p2");

  status = module.ApplyConfig(config2);
  ASSERT_EQ(status.error_code(), grpc::StatusCode::OK);

  ec.clear();
  EXPECT_FALSE(std::filesystem::exists(path0, ec));
  EXPECT_FALSE(ec);
  ec.clear();
  EXPECT_FALSE(std::filesystem::exists(path1, ec));
  EXPECT_FALSE(ec);
  ec.clear();
  EXPECT_TRUE(std::filesystem::is_symlink(path2, ec));
  EXPECT_FALSE(ec);
  ec.clear();
  EXPECT_TRUE(std::filesystem::is_symlink(path3, ec));
  EXPECT_FALSE(ec);
}

// 验证 ApplyConfig 空配置会清理已创建端口。
TEST(COMMockTest, ApplyConfigEmptyClearsPorts) {
  if (!HasSocat()) {
    GTEST_SKIP() << "socat 未安装或不在 PATH 中";
  }
  TempDir dir("commock_test");
  ASSERT_FALSE(dir.path().empty());

  const auto path0 = dir.path() / "COMMock0";
  const auto path1 = dir.path() / "COMMock1";

  COMMock::COMMock module;
  COMMockProto::COMMockConfig config;
  auto *port0 = config.add_ports();
  port0->set_name("p0");
  port0->set_dev_path(path0.string());
  port0->set_peer_name("p1");
  auto *port1 = config.add_ports();
  port1->set_name("p1");
  port1->set_dev_path(path1.string());
  port1->set_peer_name("p0");

  auto status = module.ApplyConfig(config);
  ASSERT_EQ(status.error_code(), grpc::StatusCode::OK);

  std::error_code ec;
  ASSERT_TRUE(std::filesystem::is_symlink(path0, ec));
  ASSERT_FALSE(ec);
  ec.clear();
  ASSERT_TRUE(std::filesystem::is_symlink(path1, ec));
  ASSERT_FALSE(ec);

  COMMockProto::COMMockConfig empty;
  status = module.ApplyConfig(empty);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::OK);

  ec.clear();
  EXPECT_FALSE(std::filesystem::exists(path0, ec));
  EXPECT_FALSE(ec);
  ec.clear();
  EXPECT_FALSE(std::filesystem::exists(path1, ec));
  EXPECT_FALSE(ec);
}
