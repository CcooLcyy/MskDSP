#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <chrono>
#include <stop_token>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "DataCenter.grpc.pb.h"
#include "ModuleInterface.h"
#include "ModuleManager.pb.h"

extern "C" ModuleInterface::ModuleInterface* create();
extern "C" bool GetModuleManifestPb(const uint8_t** data, size_t* size);

// 验证：IEC61850动态库通过统一工厂导出可由ModuleManager管理的模块对象。
TEST(IEC61850ManifestTest, ExportsModuleFactory) {
  std::unique_ptr<ModuleInterface::ModuleInterface> module(create());

  ASSERT_NE(module, nullptr);
  EXPECT_EQ(module->metaData().name, "IEC61850");
}

// 验证：IEC61850 manifest可解析且不把可降级的DataCenter声明为硬依赖。
TEST(IEC61850ManifestTest, ExportsManifestWithoutHardDependencies) {
  EXPECT_FALSE(GetModuleManifestPb(nullptr, nullptr));

  const uint8_t* data = nullptr;
  size_t size = 0;
  ASSERT_TRUE(GetModuleManifestPb(&data, &size));
  ASSERT_NE(data, nullptr);
  ASSERT_GT(size, 0u);

  ModuleManagerProto::ModuleManifest manifest;
  ASSERT_TRUE(manifest.ParseFromArray(data, static_cast<int>(size)));
  EXPECT_EQ(manifest.module_name(), "IEC61850");
  EXPECT_EQ(manifest.version().version(), "0.0.1");
  EXPECT_EQ(manifest.dependencies_size(), 0);
}

// 验证：IEC61850模块启动时注册DataCenter CommandExecutor，空请求返回参数错误而不是未实现。
TEST(IEC61850ManifestTest, RegistersDataCenterCommandExecutorService) {
  std::unique_ptr<ModuleInterface::ModuleInterface> module(create());
  ASSERT_NE(module, nullptr);

  std::stop_source stopSource;
  std::jthread moduleThread([&]() { module->start(stopSource.get_token()); });
  const auto channel = grpc::CreateChannel(
      module->metaData().innerGRPCServer, grpc::InsecureChannelCredentials());
  const bool connected = channel->WaitForConnected(
      std::chrono::system_clock::now() + std::chrono::seconds(3));

  grpc::Status status;
  if (connected) {
    auto stub = DataCenterProto::CommandExecutor::NewStub(channel);
    grpc::ClientContext context;
    DataCenterProto::ExecuteCommandRequest request;
    DataCenterProto::ExecuteCommandResponse response;
    status = stub->ExecuteCommand(&context, request, &response);
  }

  stopSource.request_stop();
  moduleThread.join();

  ASSERT_TRUE(connected);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
