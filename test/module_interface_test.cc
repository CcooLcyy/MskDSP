#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ModbusRTU.grpc.pb.h"
#include "Logger.h"
#include "ModuleInterface.h"

namespace {
namespace fs = std::filesystem;

class TestModule final : public ModuleInterface::ModuleInterface {
public:
  void start(std::stop_token) override {}
  void InitForTest(const LibInfo &libInfo) { initLibInfo(libInfo); }
  void BuildServers(const std::shared_ptr<grpc::Service> &service) { grpcServerBuilder(service); }
  void Shutdown() { shutdownServers(); }
  void ReservePortForTest(std::string address) { reservePort(std::move(address)); }
  void ReleasePortForTest(std::string address) { releasePort(std::move(address)); }
};

class ModbusRTUListLinksService final : public ModbusRTUProto::ModbusRTUService::Service {
public:
  grpc::Status ListLinks(grpc::ServerContext *, const ModbusRTUProto::Empty *, ModbusRTUProto::ListLinksResponse *) override {
    LOG_INFO("ModbusRTU ListLinks 测试服务已响应");
    return grpc::Status::OK;
  }
};

fs::path SocketDir() { return fs::path("socket"); }
fs::path ConfDir() { return fs::path("conf"); }

void ResetTestEnv() {
  fs::remove_all(SocketDir());
  fs::remove_all(ConfDir());
}

std::string LoopbackAddress(const std::string &addr) {
  const auto pos = addr.find(':');
  if (pos == std::string::npos) {
    return {};
  }
  return std::string("127.0.0.1") + addr.substr(pos);
}
}  // 命名空间结束

// 验证：initLibInfo 会创建 socket 目录，并在存在同名 sock 文件时将其删除后再生成 inner_grpc_server。
TEST(ModuleInterfaceTest, InitLibInfoCreatesSocketDirAndRemovesExistingSockFile) {
  ResetTestEnv();

  const std::string moduleName = "ModuleInterfaceTestA";
  fs::create_directories(SocketDir());
  const auto staleSock = SocketDir() / (moduleName + ".sock");
  std::ofstream(staleSock).put('\n');
  ASSERT_TRUE(fs::exists(staleSock));

  TestModule module;
  const LibInfo libInfo{
      .VERSION_MAJOR = "0",
      .VERSION_MINOR = "0",
      .VERSION_PATCH = "1",
      .VERSION = "0.0.1",
      .LIB_NAME = moduleName,
  };
  module.InitForTest(libInfo);

  EXPECT_TRUE(fs::exists(SocketDir()));
  EXPECT_FALSE(fs::exists(staleSock));

  const auto meta = module.metaData();
  EXPECT_TRUE(meta.innerGRPCServer.rfind("unix:", 0) == 0);
  EXPECT_NE(meta.innerGRPCServer.find(moduleName + ".sock"), std::string::npos);
  EXPECT_TRUE(meta.outerGRPCServer.rfind("0.0.0.0:", 0) == 0);
}

// 验证：grpcServerBuilder 可启动服务并处理一次 RPC；shutdownServers 可正常停止服务线程且可重复调用。
TEST(ModuleInterfaceTest, GrpcServerBuilderServesListLinksAndShutdownServersCleansUp) {
  ResetTestEnv();

  TestModule module;
  const LibInfo libInfo{
      .VERSION_MAJOR = "0",
      .VERSION_MINOR = "0",
      .VERSION_PATCH = "1",
      .VERSION = "0.0.1",
      .LIB_NAME = "ModuleInterfaceTestB",
  };
  module.InitForTest(libInfo);

  auto service = std::make_shared<ModbusRTUListLinksService>();
  module.BuildServers(service);

  const auto addr = LoopbackAddress(module.metaData().outerGRPCServer);
  ASSERT_FALSE(addr.empty());

  auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
  ASSERT_TRUE(channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(2)));

  auto stub = ModbusRTUProto::ModbusRTUService::NewStub(channel);
  grpc::ClientContext ctx;
  ModbusRTUProto::Empty req;
  ModbusRTUProto::ListLinksResponse resp;
  const auto status = stub->ListLinks(&ctx, req, &resp);
  EXPECT_TRUE(status.ok());

  module.Shutdown();
  module.Shutdown();
}

// 验证：reservePort/releasePort 对不含 ':' 的地址应直接返回，不影响进程稳定性。
TEST(ModuleInterfaceTest, ReserveAndReleasePortIgnoreAddressesWithoutColon) {
  TestModule module;
  module.ReservePortForTest("invalid");
  module.ReleasePortForTest("invalid");

  module.ReservePortForTest("0.0.0.0:7001");
  module.ReleasePortForTest("0.0.0.0:7001");
}
