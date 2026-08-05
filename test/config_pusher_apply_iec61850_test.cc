#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "ConfigPusherApplyIec61850.h"
#include "IEC61850_mock.grpc.pb.h"

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mskdsp-config-pusher-iec61850-test-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  void Write(const std::string& name, const std::string& content) const {
    std::ofstream stream(path_ / name, std::ios::binary | std::ios::trunc);
    stream << content;
  }

  std::filesystem::path configFile() const { return path_ / "iec61850.jsonc"; }

private:
  std::filesystem::path path_;
};

ConfigPusherProto::Iec61850Config MakeConfig() {
  ConfigPusherProto::Iec61850Config config;
  auto* model = config.add_models();
  model->set_model_name("station-model");
  model->set_scl_file("station.scd");
  auto* target = config.add_ieds();
  target->mutable_config()->set_conn_name("line-1");
  target->mutable_config()->set_model_name("station-model");
  target->mutable_config()->set_ied_name("IED1");
  return config;
}

// 验证：IEC61850 gRPC stub为空时下发失败。
TEST(ConfigPusherApplyIec61850Test, NullStubReturnsFalse) {
  TemporaryDirectory directory;
  EXPECT_FALSE(ConfigPusher::applyIec61850Config(
      MakeConfig(), directory.configFile(), nullptr));
}

// 验证：任一SCL文件缺失时不会发出会修改目标态的RPC。
TEST(ConfigPusherApplyIec61850Test, MissingSclFileDoesNotCallRpc) {
  TemporaryDirectory directory;
  auto stub = std::make_unique<IEC61850Proto::MockIEC61850ServiceStub>();
  EXPECT_CALL(*stub, ApplyTargetConfig(testing::_, testing::_, testing::_)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyIec61850Config(
      MakeConfig(), directory.configFile(), stub.get()));
}

// 验证：重复IED连接名在读取完成后、发出RPC前被拒绝。
TEST(ConfigPusherApplyIec61850Test, DuplicateIedNameDoesNotCallRpc) {
  TemporaryDirectory directory;
  directory.Write("station.scd", "<SCL/>");
  auto config = MakeConfig();
  *config.add_ieds() = config.ieds(0);
  auto stub = std::make_unique<IEC61850Proto::MockIEC61850ServiceStub>();
  EXPECT_CALL(*stub, ApplyTargetConfig(testing::_, testing::_, testing::_)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyIec61850Config(
      config, directory.configFile(), stub.get()));
}

// 验证：完整目标态序列化大小超过上限时在RPC前确定性拒绝。
TEST(ConfigPusherApplyIec61850Test, SerializedTargetOverLimitDoesNotCallRpc) {
  TemporaryDirectory directory;
  directory.Write("station.scd", "<SCL/>");
  auto stub = std::make_unique<IEC61850Proto::MockIEC61850ServiceStub>();
  EXPECT_CALL(*stub, ApplyTargetConfig(testing::_, testing::_, testing::_)).Times(0);

  EXPECT_FALSE(ConfigPusher::applyIec61850Config(
      MakeConfig(), directory.configFile(), stub.get(), 1));
}

// 验证：相对SCL路径基于jsonc目录读取，并以单次聚合RPC下发完整目标态。
TEST(ConfigPusherApplyIec61850Test, ReadsRelativeSclAndAppliesSingleTargetRequest) {
  TemporaryDirectory directory;
  const std::string scl = "<SCL><IED name=\"IED1\"/></SCL>";
  directory.Write("station.scd", scl);
  auto stub = std::make_unique<IEC61850Proto::MockIEC61850ServiceStub>();
  EXPECT_CALL(*stub, ApplyTargetConfig(testing::_, testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&scl](grpc::ClientContext*,
                 const IEC61850Proto::ApplyTargetConfigRequest& request,
                 IEC61850Proto::ApplyTargetConfigResponse* response) {
            if (request.models_size() != 1 || request.ieds_size() != 1) {
              ADD_FAILURE() << "IEC61850目标态数量不符合预期";
              return grpc::Status(grpc::StatusCode::INTERNAL,
                                  "测试请求数量不符合预期");
            }
            EXPECT_EQ(request.models(0).model_name(), "station-model");
            EXPECT_EQ(request.models(0).source_name(), "station.scd");
            EXPECT_EQ(request.models(0).content(), scl);
            EXPECT_EQ(request.ieds(0).config().conn_name(), "line-1");
            response->add_models()->set_model_name("station-model");
            response->add_ieds()->mutable_config()->set_conn_name("line-1");
            return grpc::Status::OK;
          }));

  EXPECT_TRUE(ConfigPusher::applyIec61850Config(
      MakeConfig(), directory.configFile(), stub.get()));
}

// 验证：空IEC61850目标态仍调用聚合RPC，用于清理模块中的旧模型和IED配置。
TEST(ConfigPusherApplyIec61850Test, EmptyTargetStillCallsRpc) {
  TemporaryDirectory directory;
  ConfigPusherProto::Iec61850Config config;
  auto stub = std::make_unique<IEC61850Proto::MockIEC61850ServiceStub>();
  EXPECT_CALL(*stub, ApplyTargetConfig(testing::_, testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [](grpc::ClientContext*,
             const IEC61850Proto::ApplyTargetConfigRequest& request,
             IEC61850Proto::ApplyTargetConfigResponse*) {
            EXPECT_EQ(request.models_size(), 0);
            EXPECT_EQ(request.ieds_size(), 0);
            return grpc::Status::OK;
          }));

  EXPECT_TRUE(ConfigPusher::applyIec61850Config(
      config, directory.configFile(), stub.get()));
}

// 验证：模块拒绝完整目标态时ConfigPusher返回失败。
TEST(ConfigPusherApplyIec61850Test, RpcFailureReturnsFalse) {
  TemporaryDirectory directory;
  directory.Write("station.scd", "<SCL/>");
  auto stub = std::make_unique<IEC61850Proto::MockIEC61850ServiceStub>();
  EXPECT_CALL(*stub, ApplyTargetConfig(testing::_, testing::_, testing::_))
      .WillOnce(testing::Return(
          grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "模型错误")));

  EXPECT_FALSE(ConfigPusher::applyIec61850Config(
      MakeConfig(), directory.configFile(), stub.get()));
}

}  // namespace
