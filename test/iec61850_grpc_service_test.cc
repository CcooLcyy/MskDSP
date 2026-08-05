#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

#include <grpcpp/server_context.h>

#include "DataCenter.grpc.pb.h"
#include "IEC61850CommandExecutorService.h"
#include "IEC61850GrpcService.h"
#include "IEC61850Manager.h"

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mskdsp-iec61850-grpc-test-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  std::filesystem::path database() const { return path_ / "config.db"; }

private:
  std::filesystem::path path_;
};

// 验证：服务未绑定Manager时返回FAILED_PRECONDITION而不是访问空指针。
TEST(IEC61850GrpcServiceTest, RejectsCallsBeforeManagerIsBound) {
  IEC61850::IEC61850GrpcServiceImpl service;
  grpc::ServerContext context;
  IEC61850Proto::Empty request;
  IEC61850Proto::ListModelsResponse response;

  const auto status = service.ListModels(&context, &request, &response);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：ImportScl和ListModels会委派给Manager并返回持久化模型摘要。
TEST(IEC61850GrpcServiceTest, DelegatesModelOperationsToManager) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  IEC61850::IEC61850GrpcServiceImpl service;
  service.SetManager(&manager);
  grpc::ServerContext context;
  IEC61850Proto::ImportSclRequest request;
  request.set_model_name("station-model");
  request.set_source_name("station.cid");
  request.set_content(R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <IED name="IED1"/>
  <DataTypeTemplates/>
</SCL>)xml");
  IEC61850Proto::ImportSclResponse response;

  ASSERT_TRUE(service.ImportScl(&context, &request, &response).ok());
  EXPECT_EQ(response.summary().ied_count(), 1u);
  IEC61850Proto::Empty empty;
  IEC61850Proto::ListModelsResponse models;
  ASSERT_TRUE(service.ListModels(&context, &empty, &models).ok());
  ASSERT_EQ(models.models_size(), 1);
  EXPECT_EQ(models.models(0).model_name(), "station-model");
}

// 验证：空请求或空响应被服务层明确拒绝。
TEST(IEC61850GrpcServiceTest, RejectsNullRequestOrResponse) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  IEC61850::IEC61850GrpcServiceImpl service;
  service.SetManager(&manager);
  grpc::ServerContext context;
  IEC61850Proto::ImportSclRequest request;
  IEC61850Proto::ImportSclResponse response;

  EXPECT_EQ(service.ImportScl(&context, nullptr, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(service.ImportScl(&context, &request, nullptr).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：同步命令服务未绑定Manager时返回FAILED_PRECONDITION。
TEST(IEC61850GrpcServiceTest, RejectsCommandBeforeManagerIsBound) {
  IEC61850::IEC61850CommandExecutorServiceImpl service;
  grpc::ServerContext context;
  DataCenterProto::ExecuteCommandRequest request;
  DataCenterProto::ExecuteCommandResponse response;

  const auto status = service.ExecuteCommand(&context, &request, &response);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：同步命令服务绑定Manager后将空请求交给Manager校验。
TEST(IEC61850GrpcServiceTest, DelegatesCommandValidationToManager) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  IEC61850::IEC61850CommandExecutorServiceImpl service;
  service.SetManager(&manager);
  grpc::ServerContext context;
  DataCenterProto::ExecuteCommandRequest request;
  DataCenterProto::ExecuteCommandResponse response;

  const auto status = service.ExecuteCommand(&context, &request, &response);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证模块关闭后同步命令不会再次进入Manager或协议栈。
TEST(IEC61850GrpcServiceTest, RejectsCommandAfterManagerShutdown) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  manager.Shutdown();
  IEC61850::IEC61850CommandExecutorServiceImpl service;
  service.SetManager(&manager);
  grpc::ServerContext context;
  DataCenterProto::ExecuteCommandRequest request;
  DataCenterProto::ExecuteCommandResponse response;

  const auto status = service.ExecuteCommand(&context, &request, &response);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

}  // namespace
