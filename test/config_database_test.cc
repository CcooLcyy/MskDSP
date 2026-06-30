#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "DataCenter.pb.h"
#include "mskdsp/ConfigDatabase.h"
#include "mskdsp/detail/ProtoSqliteStore.hpp"

namespace {
class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    path_ = base / ("config_database_test_tmp_" + std::to_string(ts));
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

grpc::Status ValidateConnectionsConfig(const DataCenterProto::ConnectionsConfig& config) {
  for (const auto& conn : config.conns()) {
    if (conn.conn_id() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_id 不能为空");
    }
    if (conn.module_name().empty() || conn.conn_name().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "稳定连接主键不能为空");
    }
  }
  return grpc::Status::OK;
}

DataCenterProto::ConnectionsConfig MakeConnections(uint32_t nextConnId) {
  DataCenterProto::ConnectionsConfig cfg;
  cfg.set_next_conn_id(nextConnId);
  auto* conn = cfg.add_conns();
  conn->set_conn_id(1);
  conn->set_module_name("IEC104");
  conn->set_conn_name("104主站");
  return cfg;
}

bool TraceContains(const std::vector<std::string>& traces, std::string_view text) {
  for (const auto& trace : traces) {
    if (trace.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}
}  // namespace

// 验证：SQLite 配置库可以保存并读取二进制 payload，且能按模块和配置项查询痕迹。
TEST(ConfigDatabaseTest, SaveLoadAndDetectBlob) {
  ScopedTempDir dir;
  mskdsp::ConfigDatabase db(dir.path() / "config.db");

  std::string payload("binary\0payload", 14);
  ASSERT_TRUE(db.SaveBlob("IEC104", "links", "IEC104Proto.LinksConfig", payload).ok());

  bool found = false;
  std::string loaded;
  ASSERT_TRUE(db.LoadBlob("IEC104", "links", &loaded, &found).ok());
  EXPECT_TRUE(found);
  EXPECT_EQ(loaded, payload);

  bool hasAny = false;
  ASSERT_TRUE(db.HasAnyBlob("IEC104", {"links", "point_tables"}, &hasAny).ok());
  EXPECT_TRUE(hasAny);
}

// 验证：protobuf SQLite Store 没有 SQLite 记录时直接返回空配置，不再读取旧 pb。
TEST(ConfigDatabaseTest, ProtoSqliteStoreReturnsEmptyWhenSqliteRowMissing) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "conf" / "config.db";
  std::vector<std::string> traces;

  mskdsp::detail::ProtoSqliteStore<DataCenterProto::ConnectionsConfig> store(
      configDbPath,
      "DataCenter",
      "connections",
      "DataCenterProto.ConnectionsConfig",
      ValidateConnectionsConfig,
      [&traces](const std::string& message) { traces.push_back(message); });

  DataCenterProto::ConnectionsConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_EQ(loaded.next_conn_id(), 0u);
  EXPECT_EQ(loaded.conns_size(), 0);
  EXPECT_TRUE(TraceContains(traces, "SQLite 配置数据库不存在"));
  EXPECT_TRUE(TraceContains(traces, "SQLite 配置项不存在，返回空配置"));
  EXPECT_TRUE(TraceContains(traces, "db_path=" + configDbPath.string()));
  EXPECT_TRUE(TraceContains(traces, "module_name=DataCenter"));
  EXPECT_TRUE(TraceContains(traces, "config_key=connections"));
  EXPECT_TRUE(TraceContains(traces, "found=false"));
  EXPECT_TRUE(TraceContains(traces, "payload_size=0"));
}

// 验证：protobuf SQLite Store 保存和读取都只通过 SQLite，不创建旧 pb 文件。
TEST(ConfigDatabaseTest, ProtoSqliteStoreSavesAndLoadsFromSqliteOnly) {
  ScopedTempDir dir;
  const auto configDbPath = dir.path() / "conf" / "config.db";
  const auto oldPbPath = dir.path() / "conf" / "IEC104" / "links.pb";
  std::vector<std::string> traces;
  mskdsp::detail::ProtoSqliteStore<DataCenterProto::ConnectionsConfig> store(
      configDbPath,
      "DataCenter",
      "connections",
      "DataCenterProto.ConnectionsConfig",
      ValidateConnectionsConfig,
      [&traces](const std::string& message) { traces.push_back(message); });
  auto updated = MakeConnections(20);
  ASSERT_TRUE(store.Save(updated).ok());
  EXPECT_FALSE(std::filesystem::exists(oldPbPath));
  EXPECT_TRUE(std::filesystem::exists(configDbPath));

  DataCenterProto::ConnectionsConfig reloaded;
  ASSERT_TRUE(store.Load(&reloaded).ok());
  EXPECT_EQ(reloaded.next_conn_id(), 20u);
  EXPECT_TRUE(TraceContains(traces, "SQLite 配置保存完成"));
  EXPECT_TRUE(TraceContains(traces, "SQLite 配置加载完成"));
  EXPECT_TRUE(TraceContains(traces, "db_path=" + configDbPath.string()));
  EXPECT_TRUE(TraceContains(traces, "module_name=DataCenter"));
  EXPECT_TRUE(TraceContains(traces, "config_key=connections"));
  EXPECT_TRUE(TraceContains(traces, "found=true"));
}
