#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "ConfigPusher.h"
#include "ModuleManager_mock.grpc.pb.h"

namespace {
using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

using ConfigPusherClass = ConfigPusher::ConfigPusher;
}  // namespace

namespace ConfigPusher {
class ConfigPusherTestPeer {
public:
  static void ApplyConfig(ConfigPusher &pusher) { pusher.applyConfig(); }
};
}  // namespace ConfigPusher

namespace {
using ConfigPusher::ConfigPusherTestPeer;

std::filesystem::path MakeTempDir() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  auto path = std::filesystem::temp_directory_path() / ("mskdsp_config_pusher_test_" + std::to_string(now));
  std::filesystem::create_directories(path);
  return path;
}

void WriteFile(const std::filesystem::path &path, const std::string &content) {
  std::ofstream ofs(path, std::ios::out | std::ios::binary);
  ofs << content;
}

ModuleManagerProto::ModuleInfo MakeModuleInfo(const std::string &name) {
  ModuleManagerProto::ModuleInfo info;
  info.set_module_name(name);
  return info;
}

}  // namespace

// 验证：无配置文件时直接返回，不触发模块管理请求。
TEST(ConfigPusherApplyConfigTest, NoConfigReturnsEarly) {
  auto stub = std::make_shared<ModuleManagerProto::MockModuleManageStub>();
  EXPECT_CALL(*stub, GetModuleInfo(_, _, _)).Times(0);

  ConfigPusherClass pusher;
  pusher.setModuleManagerStub(stub);
  pusher.setConfigDirForTest(MakeTempDir());

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
