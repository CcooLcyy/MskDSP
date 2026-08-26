#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ControlOrchestratorManager.h"
#include "support/FakeDataCenter.hpp"

namespace {

class ScopedTempDir {
public:
  ScopedTempDir() {
    path_ = std::filesystem::current_path() /
        ("control_orchestrator_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
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

ControlOrchestratorProto::WorkflowConfig MakeSequence() {
  ControlOrchestratorProto::WorkflowConfig config;
  config.set_sequence_name("逆变器遥调前置合闸");
  auto *prepare = config.add_steps();
  prepare->set_step_name("遥控合");
  prepare->mutable_source()->set_module_name("ModbusRTU");
  prepare->mutable_source()->set_conn_name("逆变器1");
  prepare->mutable_source()->set_conn_id(1);
  prepare->mutable_source()->set_tag("remote_close");
  prepare->mutable_value()->set_bool_value(true);
  prepare->set_timeout_ms(1000);
  prepare->set_delay_after_ms(1);
  auto *adjust = config.add_steps();
  adjust->set_step_name("有功遥调");
  adjust->mutable_source()->set_module_name("ModbusRTU");
  adjust->mutable_source()->set_conn_name("逆变器1");
  adjust->mutable_source()->set_conn_id(1);
  adjust->mutable_source()->set_tag("active_power");
  adjust->set_use_trigger_value(true);
  adjust->set_timeout_ms(1000);
  return config;
}

DataCenterProto::ExecuteCommandResponse Accepted() {
  DataCenterProto::ExecuteCommandResponse response;
  response.set_status(DataCenterProto::COMMAND_ACCEPTED);
  return response;
}

}  // namespace

// 验证：新增编排时拒绝空名称、空步骤、重复步骤名及非法步骤数量。
TEST(ControlOrchestratorManagerTest, RejectsInvalidSequenceConfig) {
  ScopedTempDir tempDir;
  ControlOrchestrator::SequenceManager manager(tempDir.path() / "config.db");
  ControlOrchestratorProto::WorkflowConfig config;
  ControlOrchestratorProto::WorkflowConfig out;
  auto status = manager.UpsertSequence(config, false, &out);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  config.set_sequence_name("空步骤");
  status = manager.UpsertSequence(config, false, &out);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：编排步骤按配置顺序执行，use_trigger_value 步骤复用执行请求值。
TEST(ControlOrchestratorManagerTest, ExecutesStepsInOrderAndUsesTriggerValue) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  std::mutex orderMu;
  std::vector<std::string> order;
  ON_CALL(*stub, ExecuteCommand(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke(
          [&orderMu, &order](grpc::ClientContext*, const DataCenterProto::ExecuteCommandRequest &request,
                             DataCenterProto::ExecuteCommandResponse *response) {
            std::lock_guard<std::mutex> lock(orderMu);
            order.push_back(request.src().tag());
            if (request.src().tag() == "active_power") {
              EXPECT_TRUE(request.value().has_double_value());
              EXPECT_DOUBLE_EQ(request.value().double_value(), 12.5);
            }
            *response = Accepted();
            return grpc::Status::OK;
          }));

  ControlOrchestrator::SequenceManager manager(tempDir.path() / "config.db");
  manager.setDataCenterStub(stub);
  auto config = MakeSequence();
  ControlOrchestratorProto::WorkflowConfig out;
  ASSERT_TRUE(manager.UpsertSequence(config, false, &out).ok());

  ControlOrchestratorProto::ExecuteSequenceRequest request;
  request.set_sequence_name(config.sequence_name());
  request.mutable_trigger_value()->set_double_value(12.5);
  ControlOrchestratorProto::ExecuteSequenceResponse response;
  ASSERT_TRUE(manager.ExecuteSequence(request, &response).ok());
  EXPECT_TRUE(response.accepted());
  EXPECT_EQ(response.executed_steps(), 2u);
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], "remote_close");
  EXPECT_EQ(order[1], "active_power");
}

// 验证：步骤失败后立即停止，后续步骤不会调用 DataCenter。
TEST(ControlOrchestratorManagerTest, StopsAfterFailedStep) {
  ScopedTempDir tempDir;
  FakeDataCenterState state;
  state.RejectCommandForTag("remote_close", "设备未处于远方状态");
  auto stub = MakeStub(&state);
  ControlOrchestrator::SequenceManager manager(tempDir.path() / "config.db");
  manager.setDataCenterStub(stub);
  auto config = MakeSequence();
  ControlOrchestratorProto::WorkflowConfig out;
  ASSERT_TRUE(manager.UpsertSequence(config, false, &out).ok());

  ControlOrchestratorProto::ExecuteSequenceRequest request;
  request.set_sequence_name(config.sequence_name());
  request.mutable_trigger_value()->set_double_value(12.5);
  ControlOrchestratorProto::ExecuteSequenceResponse response;
  ASSERT_TRUE(manager.ExecuteSequence(request, &response).ok());
  EXPECT_FALSE(response.accepted());
  EXPECT_EQ(response.executed_steps(), 0u);
  EXPECT_EQ(response.failed_step_index(), 1u);
  EXPECT_EQ(state.GetCommandCount(1, "active_power"), 0u);
}

// 验证：同一编排的并发执行互斥，第二次执行不会与第一次交错。
TEST(ControlOrchestratorManagerTest, SerializesConcurrentExecutionPerSequence) {
  ScopedTempDir tempDir;
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  std::mutex mu;
  std::vector<std::string> order;
  int active = 0;
  int maxActive = 0;
  ON_CALL(*stub, ExecuteCommand(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke(
          [&mu, &order, &active, &maxActive](grpc::ClientContext*,
                                               const DataCenterProto::ExecuteCommandRequest &request,
                                               DataCenterProto::ExecuteCommandResponse *response) {
            {
              std::lock_guard<std::mutex> lock(mu);
              ++active;
              maxActive = std::max(maxActive, active);
              order.push_back(request.src().tag());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            {
              std::lock_guard<std::mutex> lock(mu);
              --active;
              order.push_back(request.src().tag() + "_完成");
            }
            *response = Accepted();
            return grpc::Status::OK;
          }));

  ControlOrchestrator::SequenceManager manager(tempDir.path() / "config.db");
  manager.setDataCenterStub(stub);
  auto config = MakeSequence();
  ControlOrchestratorProto::WorkflowConfig out;
  ASSERT_TRUE(manager.UpsertSequence(config, false, &out).ok());

  auto execute = [&manager]() {
    ControlOrchestratorProto::ExecuteSequenceRequest request;
    request.set_sequence_name("逆变器遥调前置合闸");
    request.mutable_trigger_value()->set_double_value(12.5);
    ControlOrchestratorProto::ExecuteSequenceResponse response;
    EXPECT_TRUE(manager.ExecuteSequence(request, &response).ok());
    EXPECT_TRUE(response.accepted());
  };
  std::thread first(execute);
  std::thread second(execute);
  first.join();
  second.join();
  EXPECT_EQ(maxActive, 1);
}

// 验证：配置写入后可由新的管理器实例恢复。
TEST(ControlOrchestratorManagerTest, PersistsAndRestoresSequences) {
  ScopedTempDir tempDir;
  const auto dbPath = tempDir.path() / "config.db";
  auto config = MakeSequence();
  {
    ControlOrchestrator::SequenceManager manager(dbPath);
    ControlOrchestratorProto::WorkflowConfig out;
    ASSERT_TRUE(manager.UpsertSequence(config, false, &out).ok());
  }
  ControlOrchestrator::SequenceManager restored(dbPath);
  ASSERT_TRUE(restored.LoadPersistedConfig().ok());
  ControlOrchestratorProto::WorkflowConfig out;
  ASSERT_TRUE(restored.GetSequence(config.sequence_name(), &out).ok());
  EXPECT_EQ(out.steps_size(), 2);
  EXPECT_EQ(out.steps(0).step_name(), "遥控合");
}

// 验证：编排总超时覆盖步骤间延时，并在后续步骤开始前返回失败。
TEST(ControlOrchestratorManagerTest, StopsWhenSequenceTimeoutExpires) {
  ScopedTempDir tempDir;
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  ON_CALL(*stub, ExecuteCommand(::testing::_, ::testing::_, ::testing::_))
      .WillByDefault(::testing::Invoke(
          [](grpc::ClientContext*, const DataCenterProto::ExecuteCommandRequest &,
             DataCenterProto::ExecuteCommandResponse *response) {
            *response = Accepted();
            return grpc::Status::OK;
          }));

  ControlOrchestrator::SequenceManager manager(tempDir.path() / "config.db");
  manager.setDataCenterStub(stub);
  auto config = MakeSequence();
  config.mutable_steps(0)->set_delay_after_ms(50);
  ControlOrchestratorProto::WorkflowConfig out;
  ASSERT_TRUE(manager.UpsertSequence(config, false, &out).ok());

  ControlOrchestratorProto::ExecuteSequenceRequest request;
  request.set_sequence_name(config.sequence_name());
  request.set_timeout_ms(5);
  request.mutable_trigger_value()->set_double_value(12.5);
  ControlOrchestratorProto::ExecuteSequenceResponse response;
  ASSERT_TRUE(manager.ExecuteSequence(request, &response).ok());
  EXPECT_FALSE(response.accepted());
  EXPECT_EQ(response.executed_steps(), 1u);
  EXPECT_EQ(response.failed_step_index(), 2u);
  EXPECT_EQ(response.reason(), "编排总超时");
}
