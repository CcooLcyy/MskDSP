#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <tuple>

#include "DataCenterRouteStore.h"

namespace {
using DataCenter::DataCenterRouteStore;

class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    path_ = base / ("data_center_route_store_test_tmp_" + std::to_string(ts));
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

using RouteKey = std::tuple<uint32_t, std::string, uint32_t, std::string>;

std::set<RouteKey> ToSet(const DataCenterProto::RoutesConfig& cfg) {
  std::set<RouteKey> out;
  for (const auto& route : cfg.routes()) {
    out.emplace(route.src().conn_id(), route.src().tag(), route.dst().conn_id(), route.dst().tag());
  }
  return out;
}

DataCenterProto::RoutesConfig MakeConfig(std::initializer_list<std::tuple<uint32_t, const char*, uint32_t, const char*>> routes) {
  DataCenterProto::RoutesConfig cfg;
  for (const auto& [srcConnId, srcTag, dstConnId, dstTag] : routes) {
    auto* route = cfg.add_routes();
    route->mutable_src()->set_conn_id(srcConnId);
    route->mutable_src()->set_tag(srcTag);
    route->mutable_dst()->set_conn_id(dstConnId);
    route->mutable_dst()->set_tag(dstTag);
  }
  return cfg;
}
}  // namespace

TEST(DataCenterRouteStoreTest, LoadReturnsEmptyWhenNoFiles) {
  ScopedTempDir dir;
  DataCenterRouteStore store(dir.path() / "routes.pb");

  DataCenterProto::RoutesConfig cfg;
  ASSERT_TRUE(store.Load(&cfg).ok());
  EXPECT_EQ(cfg.routes_size(), 0);
}

TEST(DataCenterRouteStoreTest, SaveAndLoadRoundtrip) {
  ScopedTempDir dir;
  DataCenterRouteStore store(dir.path() / "routes.pb");

  auto cfg = MakeConfig({
      {1, "源点", 2, "目的点"},
      {1, "源点", 3, "目的点2"},
  });
  ASSERT_TRUE(store.Save(cfg).ok());

  DataCenterProto::RoutesConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(ToSet(loaded), ToSet(cfg));
}

TEST(DataCenterRouteStoreTest, SaveRejectsInvalidConfig) {
  ScopedTempDir dir;
  DataCenterRouteStore store(dir.path() / "routes.pb");

  DataCenterProto::RoutesConfig cfg;
  auto* route = cfg.add_routes();
  route->mutable_src()->set_conn_id(0);
  route->mutable_src()->set_tag("x");
  route->mutable_dst()->set_conn_id(2);
  route->mutable_dst()->set_tag("y");

  auto status = store.Save(cfg);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(DataCenterRouteStoreTest, LoadFallsBackToBackupWhenMainCorruptedAndRestoresMainBestEffort) {
  ScopedTempDir dir;
  const auto base = dir.path() / "routes.pb";
  DataCenterRouteStore store(base);

  auto cfg1 = MakeConfig({
      {1, "源点", 2, "目的点"},
  });
  auto cfg2 = MakeConfig({
      {1, "源点", 2, "目的点"},
      {1, "源点", 3, "目的点2"},
  });

  ASSERT_TRUE(store.Save(cfg1).ok());
  ASSERT_TRUE(store.Save(cfg2).ok());

  {
    std::ofstream ofs(base, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << "corrupt";
  }

  DataCenterProto::RoutesConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(ToSet(loaded), ToSet(cfg1));

  DataCenterProto::RoutesConfig restoredMain;
  ASSERT_TRUE(restoredMain.ParseFromString([&]() {
    std::ifstream ifs(base, std::ios::binary);
    EXPECT_TRUE(ifs.is_open());
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }()));
  EXPECT_EQ(ToSet(restoredMain), ToSet(cfg1));

  bool foundCorrupt = false;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
    const auto name = entry.path().filename().string();
    if (name.rfind("routes.pb.corrupt.", 0) == 0) {
      foundCorrupt = true;
      break;
    }
  }
  EXPECT_TRUE(foundCorrupt);
}

