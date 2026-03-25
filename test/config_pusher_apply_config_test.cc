#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ConfigPusher.h"
#include "DataCenter.grpc.pb.h"
#include "ModuleManager_mock.grpc.pb.h"

namespace {
using ::testing::_;
using ::testing::AtLeast;
using ::testing::Invoke;
using ::testing::Return;

using ConfigPusherClass = ConfigPusher::ConfigPusher;
}  // 命名空间结束

namespace ConfigPusher {
class ConfigPusherTestPeer {
public:
  static void ApplyConfig(ConfigPusher &pusher) { pusher.applyConfig(); }
};
}  // ConfigPusher 命名空间结束

namespace {
using ConfigPusher::ConfigPusherTestPeer;

class ScopedTempDir {
public:
  ScopedTempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("mskdsp_config_pusher_apply_test_" + std::to_string(now));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

class ScopedCwd {
public:
  explicit ScopedCwd(const std::filesystem::path &newCwd) :
      old_(std::filesystem::current_path()) {
    std::filesystem::current_path(newCwd);
  }

  ~ScopedCwd() { std::filesystem::current_path(old_); }

  ScopedCwd(const ScopedCwd &) = delete;
  ScopedCwd &operator=(const ScopedCwd &) = delete;

private:
  std::filesystem::path old_;
};

std::filesystem::path MakeTempDir() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  auto path = std::filesystem::temp_directory_path() / ("mskdsp_config_pusher_test_" + std::to_string(now));
  std::filesystem::create_directories(path);
  return path;
}

void WriteFile(const std::filesystem::path &path, const std::string &content) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream ofs(path, std::ios::out | std::ios::binary);
  ofs << content;
}

ModuleManagerProto::ModuleInfo MakeModuleInfo(const std::string &name) {
  ModuleManagerProto::ModuleInfo info;
  info.set_module_name(name);
  return info;
}

class FakeDataCenterService final : public DataCenterProto::DataCenterService::Service {
public:
  FakeDataCenterService() {
    auto *src = connections_.add_conns();
    src->set_conn_id(10);
    src->set_module_name("IEC104");
    src->set_conn_name("line-1");

    auto *dst = connections_.add_conns();
    dst->set_conn_id(20);
    dst->set_module_name("AGC");
    dst->set_conn_name("g-1");
  }

  grpc::Status ListConnections(grpc::ServerContext *,
                               const DataCenterProto::Empty *,
                               DataCenterProto::ListConnectionsResponse *response) override {
    if (response == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应对象为空");
    }
    *response = connections_;
    return grpc::Status::OK;
  }

  grpc::Status UpsertConnTags(grpc::ServerContext *,
                              const DataCenterProto::UpsertConnTagsRequest *request,
                              DataCenterProto::Empty *) override {
    if (request == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "连接标签请求为空");
    }
    std::lock_guard<std::mutex> lock(mu_);
    connTagsRequests_.push_back(*request);
    return grpc::Status::OK;
  }

  grpc::Status UpsertRoutes(grpc::ServerContext *,
                            const DataCenterProto::UpsertRoutesRequest *request,
                            DataCenterProto::Empty *) override {
    if (request == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "路由请求为空");
    }
    std::lock_guard<std::mutex> lock(mu_);
    routeRequests_.push_back(*request);
    return grpc::Status::OK;
  }

  std::vector<DataCenterProto::UpsertConnTagsRequest> connTagsRequests() const {
    std::lock_guard<std::mutex> lock(mu_);
    return connTagsRequests_;
  }

  std::vector<DataCenterProto::UpsertRoutesRequest> routeRequests() const {
    std::lock_guard<std::mutex> lock(mu_);
    return routeRequests_;
  }

private:
  mutable std::mutex mu_;
  DataCenterProto::ListConnectionsResponse connections_;
  std::vector<DataCenterProto::UpsertConnTagsRequest> connTagsRequests_;
  std::vector<DataCenterProto::UpsertRoutesRequest> routeRequests_;
};

}  // 命名空间结束

// 验证：无配置文件时直接返回，不触发模块管理请求。
TEST(ConfigPusherApplyConfigTest, NoConfigReturnsEarly) {
  auto stub = std::make_shared<ModuleManagerProto::MockModuleManageStub>();
  EXPECT_CALL(*stub, GetModuleInfo(_, _, _)).Times(0);

  ConfigPusherClass pusher;
  pusher.setModuleManagerStub(stub);
  pusher.setConfigDirForTest(MakeTempDir());

  ConfigPusherTestPeer::ApplyConfig(pusher);
}

// 验证：即使存在模块持久化 pb 痕迹，但缺少对应 jsonc 配置时，ConfigPusher 仍按无配置处理并直接返回。
TEST(ConfigPusherApplyConfigTest, PersistentTraceWithoutJsonStillReturnsEarly) {
  ScopedTempDir workDir;
  ScopedCwd cwd(workDir.path());

  const auto configDir = workDir.path() / "configPusher";
  std::filesystem::create_directories(configDir);

  WriteFile(workDir.path() / "conf" / "dataCenter" / "connections.pb", "trace");
  WriteFile(workDir.path() / "conf" / "IEC104" / "links.pb", "trace");
  WriteFile(workDir.path() / "conf" / "ModbusRTU" / "links.pb", "trace");
  WriteFile(workDir.path() / "conf" / "DLT645" / "links.pb", "trace");
  WriteFile(workDir.path() / "conf" / "AGC" / "groups.pb", "trace");

  auto stub = std::make_shared<ModuleManagerProto::MockModuleManageStub>();
  EXPECT_CALL(*stub, GetModuleInfo(_, _, _)).Times(0);
  EXPECT_CALL(*stub, GetRunningModuleInfo(_, _, _)).Times(0);
  EXPECT_CALL(*stub, StartModule(_, _, _)).Times(0);

  ConfigPusherClass pusher;
  pusher.setModuleManagerStub(stub);
  pusher.setConfigDirForTest(configDir);

  ConfigPusherTestPeer::ApplyConfig(pusher);
}

// 验证：获取模块信息失败时终止下发。
TEST(ConfigPusherApplyConfigTest, FetchModuleInfosFailure) {
  auto dir = MakeTempDir();
  WriteFile(dir / "DataCenter.jsonc", R"({"point_tables":[{"module_name":"A","conn_name":"B","tags":["t1"]}]})");

  auto stub = std::make_shared<ModuleManagerProto::MockModuleManageStub>();
  ON_CALL(*stub, GetModuleInfo(_, _, _))
      .WillByDefault(Return(grpc::Status(grpc::StatusCode::INTERNAL, "获取失败")));

  ConfigPusherClass pusher;
  pusher.setModuleManagerStub(stub);
  pusher.setConfigDirForTest(dir);

  ConfigPusherTestPeer::ApplyConfig(pusher);
}

// 验证：模块信息缺失时返回错误。
TEST(ConfigPusherApplyConfigTest, MissingModuleInfoStopsApply) {
  auto dir = MakeTempDir();
  WriteFile(dir / "iec104.jsonc", R"({"iec104":{"links":[{}]}})");

  auto stub = std::make_shared<ModuleManagerProto::MockModuleManageStub>();
  ON_CALL(*stub, GetModuleInfo(_, _, _))
      .WillByDefault(Invoke([](grpc::ClientContext*, const ModuleManagerProto::Empty&, ModuleManagerProto::ModuleInfos* resp) {
        resp->Clear();
        *resp->add_module_info() = MakeModuleInfo("DataCenter");
        return grpc::Status::OK;
      }));

  ConfigPusherClass pusher;
  pusher.setModuleManagerStub(stub);
  pusher.setConfigDirForTest(dir);

  ConfigPusherTestPeer::ApplyConfig(pusher);
}

// 验证：DataCenter 未运行且启动失败时终止。
TEST(ConfigPusherApplyConfigTest, StartModuleFailureStopsApply) {
  auto dir = MakeTempDir();
  WriteFile(dir / "DataCenter.jsonc", R"({"point_tables":[{"module_name":"A","conn_name":"B","tags":["t1"]}]})");

  auto stub = std::make_shared<ModuleManagerProto::MockModuleManageStub>();
  ON_CALL(*stub, GetModuleInfo(_, _, _))
      .WillByDefault(Invoke([](grpc::ClientContext*, const ModuleManagerProto::Empty&, ModuleManagerProto::ModuleInfos* resp) {
        resp->Clear();
        *resp->add_module_info() = MakeModuleInfo("DataCenter");
        return grpc::Status::OK;
      }));
  ON_CALL(*stub, GetRunningModuleInfo(_, _, _))
      .WillByDefault(Invoke([](grpc::ClientContext*, const ModuleManagerProto::Empty&, ModuleManagerProto::ModuleRunningInfos* resp) {
        resp->Clear();
        return grpc::Status::OK;
      }));
  ON_CALL(*stub, StartModule(_, _, _))
      .WillByDefault(Return(grpc::Status(grpc::StatusCode::INTERNAL, "启动失败")));

  ConfigPusherClass pusher;
  pusher.setModuleManagerStub(stub);
  pusher.setConfigDirForTest(dir);

  ConfigPusherTestPeer::ApplyConfig(pusher);
}

// 验证：等待模块启动时获取运行信息失败，提前返回。
TEST(ConfigPusherApplyConfigTest, WaitForModuleStopsOnRunningInfoFailure) {
  auto dir = MakeTempDir();
  WriteFile(dir / "DataCenter.jsonc", R"({"point_tables":[{"module_name":"A","conn_name":"B","tags":["t1"]}]})");

  auto stub = std::make_shared<ModuleManagerProto::MockModuleManageStub>();
  ON_CALL(*stub, GetModuleInfo(_, _, _))
      .WillByDefault(Invoke([](grpc::ClientContext*, const ModuleManagerProto::Empty&, ModuleManagerProto::ModuleInfos* resp) {
        resp->Clear();
        *resp->add_module_info() = MakeModuleInfo("DataCenter");
        return grpc::Status::OK;
      }));
  ON_CALL(*stub, GetRunningModuleInfo(_, _, _))
      .WillByDefault(Return(grpc::Status(grpc::StatusCode::INTERNAL, "运行信息失败")));
  ON_CALL(*stub, StartModule(_, _, _))
      .WillByDefault(Return(grpc::Status::OK));

  ConfigPusherClass pusher;
  pusher.setModuleManagerStub(stub);
  pusher.setConfigDirForTest(dir);

  ConfigPusherTestPeer::ApplyConfig(pusher);
}

// 验证：即使存在模块持久化配置文件痕迹，ConfigPusher 仍会按 jsonc 启动 DataCenter 并下发配置。
TEST(ConfigPusherApplyConfigTest, AppliesJsoncConfigAndStartsModuleEvenWhenPersistentTraceExists) {
  ScopedTempDir workDir;
  ScopedCwd cwd(workDir.path());

  const auto configDir = workDir.path() / "configPusher";
  WriteFile(configDir / "DataCenter.jsonc", R"json(
{
  "point_tables": [
    {
      "module_name": "IEC104",
      "conn_name": "line-1",
      "tags": ["P_CMD_SRC"],
      "replace": true
    },
    {
      "module_name": "AGC",
      "conn_name": "g-1",
      "tags": ["P_CMD"],
      "replace": true
    }
  ],
  "routes": {
    "replace": true,
    "routes": [
      {
        "src": { "module_name": "IEC104", "conn_name": "line-1", "tag": "P_CMD_SRC" },
        "dst": { "module_name": "AGC", "conn_name": "g-1", "tag": "P_CMD" }
      }
    ]
  }
}
)json");

  WriteFile(workDir.path() / "conf" / "dataCenter" / "connections.pb", "trace");
  WriteFile(workDir.path() / "conf" / "IEC104" / "links.pb", "trace");
  WriteFile(workDir.path() / "conf" / "AGC" / "groups.pb", "trace");

  FakeDataCenterService dataCenterService;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.RegisterService(&dataCenterService);
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  ASSERT_GT(port, 0);
  const auto dataCenterAddr = std::string("127.0.0.1:") + std::to_string(port);

  auto stub = std::make_shared<ModuleManagerProto::MockModuleManageStub>();
  EXPECT_CALL(*stub, GetModuleInfo(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *, const ModuleManagerProto::Empty &, ModuleManagerProto::ModuleInfos *resp) {
        resp->Clear();
        *resp->add_module_info() = MakeModuleInfo("DataCenter");
        return grpc::Status::OK;
      }));

  size_t runningInfoCalls = 0;
  EXPECT_CALL(*stub, GetRunningModuleInfo(_, _, _))
      .Times(AtLeast(2))
      .WillRepeatedly(Invoke([&](grpc::ClientContext *, const ModuleManagerProto::Empty &, ModuleManagerProto::ModuleRunningInfos *resp) {
        resp->Clear();
        if (runningInfoCalls++ >= 1) {
          auto *running = resp->add_module_running_info();
          running->set_module_name("DataCenter");
          running->set_inner_grpc_server(dataCenterAddr);
        }
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, StartModule(_, _, _))
      .Times(1)
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const ModuleManagerProto::ModuleInfo &info,
                          ModuleManagerProto::Empty *) {
        EXPECT_EQ(info.module_name(), "DataCenter");
        return grpc::Status::OK;
      }));

  ConfigPusherClass pusher;
  pusher.setModuleManagerStub(stub);
  pusher.setConfigDirForTest(configDir);

  ConfigPusherTestPeer::ApplyConfig(pusher);

  const auto connTagsRequests = dataCenterService.connTagsRequests();
  ASSERT_EQ(connTagsRequests.size(), 2u);
  bool sawSrcConnTags = false;
  bool sawDstConnTags = false;
  for (const auto& req : connTagsRequests) {
    EXPECT_TRUE(req.replace());
    if (req.conn_id() == 10u) {
      ASSERT_EQ(req.tags_size(), 1);
      EXPECT_EQ(req.tags(0), "P_CMD_SRC");
      sawSrcConnTags = true;
    } else if (req.conn_id() == 20u) {
      ASSERT_EQ(req.tags_size(), 1);
      EXPECT_EQ(req.tags(0), "P_CMD");
      sawDstConnTags = true;
    }
  }
  EXPECT_TRUE(sawSrcConnTags);
  EXPECT_TRUE(sawDstConnTags);

  const auto routeRequests = dataCenterService.routeRequests();
  ASSERT_EQ(routeRequests.size(), 1u);
  ASSERT_EQ(routeRequests[0].routes_size(), 1);
  EXPECT_TRUE(routeRequests[0].replace());
  EXPECT_EQ(routeRequests[0].routes(0).src().conn_id(), 10u);
  EXPECT_EQ(routeRequests[0].routes(0).src().tag(), "P_CMD_SRC");
  EXPECT_EQ(routeRequests[0].routes(0).dst().conn_id(), 20u);
  EXPECT_EQ(routeRequests[0].routes(0).dst().tag(), "P_CMD");

  server->Shutdown();
}

// 验证：即使 DataCenter.jsonc 目标态为空，ConfigPusher 仍会启动 DataCenter 并按空目标态清空旧标签/旧路由。
TEST(ConfigPusherApplyConfigTest, EmptyDataCenterJsonStillAppliesAsTargetState) {
  ScopedTempDir workDir;
  ScopedCwd cwd(workDir.path());

  const auto configDir = workDir.path() / "configPusher";
  WriteFile(configDir / "DataCenter.jsonc", R"json(
{
  "point_tables": [],
  "routes": {
    "routes": []
  }
}
)json");

  WriteFile(workDir.path() / "conf" / "dataCenter" / "connections.pb", "trace");

  FakeDataCenterService dataCenterService;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.RegisterService(&dataCenterService);
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  ASSERT_GT(port, 0);
  const auto dataCenterAddr = std::string("127.0.0.1:") + std::to_string(port);

  auto stub = std::make_shared<ModuleManagerProto::MockModuleManageStub>();
  EXPECT_CALL(*stub, GetModuleInfo(_, _, _))
      .WillOnce(Invoke([](grpc::ClientContext *, const ModuleManagerProto::Empty &, ModuleManagerProto::ModuleInfos *resp) {
        resp->Clear();
        *resp->add_module_info() = MakeModuleInfo("DataCenter");
        return grpc::Status::OK;
      }));

  size_t runningInfoCalls = 0;
  EXPECT_CALL(*stub, GetRunningModuleInfo(_, _, _))
      .Times(AtLeast(2))
      .WillRepeatedly(Invoke([&](grpc::ClientContext *, const ModuleManagerProto::Empty &, ModuleManagerProto::ModuleRunningInfos *resp) {
        resp->Clear();
        if (runningInfoCalls++ >= 1) {
          auto *running = resp->add_module_running_info();
          running->set_module_name("DataCenter");
          running->set_inner_grpc_server(dataCenterAddr);
        }
        return grpc::Status::OK;
      }));

  EXPECT_CALL(*stub, StartModule(_, _, _))
      .Times(1)
      .WillOnce(Invoke([](grpc::ClientContext *,
                          const ModuleManagerProto::ModuleInfo &info,
                          ModuleManagerProto::Empty *) {
        EXPECT_EQ(info.module_name(), "DataCenter");
        return grpc::Status::OK;
      }));

  ConfigPusherClass pusher;
  pusher.setModuleManagerStub(stub);
  pusher.setConfigDirForTest(configDir);

  ConfigPusherTestPeer::ApplyConfig(pusher);

  const auto connTagsRequests = dataCenterService.connTagsRequests();
  ASSERT_EQ(connTagsRequests.size(), 2u);
  for (const auto& req : connTagsRequests) {
    EXPECT_TRUE(req.replace());
    EXPECT_EQ(req.tags_size(), 0);
  }

  const auto routeRequests = dataCenterService.routeRequests();
  ASSERT_EQ(routeRequests.size(), 1u);
  EXPECT_TRUE(routeRequests[0].replace());
  EXPECT_EQ(routeRequests[0].routes_size(), 0);

  server->Shutdown();
}
