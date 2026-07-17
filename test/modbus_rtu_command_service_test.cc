#include <gtest/gtest.h>

#include <chrono>
#include <stop_token>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "DataCenter.grpc.pb.h"
#include "ModbusRTU.h"

// 验证：ModbusRTU 模块启动时会注册 CommandExecutor，调用不再返回 UNIMPLEMENTED。
TEST(ModbusRtuCommandServiceTest, ModuleRegistersCommandExecutorService) {
  ModbusRTU::ModbusRTU module;
  std::stop_source stopSource;
  std::jthread moduleThread([&]() { module.start(stopSource.get_token()); });

  const auto channel = grpc::CreateChannel(module.metaData().innerGRPCServer, grpc::InsecureChannelCredentials());
  const bool connected = channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(3));

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
