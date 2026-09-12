#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "BoardOutputManager.hpp"

namespace BoardIO {
namespace {

struct GpioWrite {
  uint32_t offset = 0;
  bool value = false;
};

class RecordingGpioOutputDriver final : public GpioOutputDriver {
 public:
  bool Open(const std::string& chipPath, const std::vector<GpioOutputValue>& initialValues,
            std::string* error) override {
    openedChipPath = chipPath;
    openedValues = initialValues;
    if (failOpen) {
      if (error != nullptr) {
        *error = "模拟 GPIO 申请失败";
      }
      return false;
    }
    opened = true;
    return true;
  }

  bool SetValue(uint32_t offset, bool value, std::string* error) override {
    {
      std::unique_lock lock(blockMutex);
      writes.push_back(GpioWrite{.offset = offset, .value = value});
      if (blockNextWrite) {
        blockNextWrite = false;
        writeBlocked = true;
        blockCondition.notify_all();
        blockCondition.wait(lock, [this]() { return allowBlockedWrite; });
      }
    }
    if (failWriteOffset == offset) {
      if (error != nullptr) {
        *error = "模拟 GPIO 写入失败";
      }
      return false;
    }
    return true;
  }

  bool Close(std::string*) override {
    closed = true;
    opened = false;
    return true;
  }

  void BlockNextSetValue() {
    std::lock_guard lock(blockMutex);
    blockNextWrite = true;
    writeBlocked = false;
    allowBlockedWrite = false;
  }

  bool WaitUntilSetValueBlocked() {
    std::unique_lock lock(blockMutex);
    return blockCondition.wait_for(lock, std::chrono::seconds(1),
                                   [this]() { return writeBlocked; });
  }

  void ReleaseBlockedSetValue() {
    {
      std::lock_guard lock(blockMutex);
      allowBlockedWrite = true;
    }
    blockCondition.notify_all();
  }

  bool failOpen = false;
  std::optional<uint32_t> failWriteOffset;
  bool opened = false;
  bool closed = false;
  std::string openedChipPath;
  std::vector<GpioOutputValue> openedValues;
  std::vector<GpioWrite> writes;

 private:
  std::mutex blockMutex;
  std::condition_variable blockCondition;
  bool blockNextWrite = false;
  bool writeBlocked = false;
  bool allowBlockedWrite = false;
};

DataCenterProto::ExecuteCommandRequest MakeCommand(std::string tag, bool value, uint32_t connId = 42) {
  DataCenterProto::ExecuteCommandRequest request;
  request.mutable_dst()->set_module_name("BoardIO");
  request.mutable_dst()->set_conn_name("board-do");
  request.mutable_dst()->set_conn_id(connId);
  request.mutable_dst()->set_tag(std::move(tag));
  request.mutable_value()->set_bool_value(value);
  request.set_quality(DataCenterProto::QUALITY_GOOD);
  return request;
}

// 验证：启动模块内输出功能时以一组请求原子设置 DO1、DO2 为低电平且失电继电器为高电平。
TEST(BoardOutputManagerTest, StartsWithSafeInitialOutputValues) {
  auto driver = std::make_unique<RecordingGpioOutputDriver>();
  auto* recording = driver.get();
  BoardOutputManager manager(std::move(driver));
  std::string error;

  ASSERT_TRUE(manager.Start("/dev/test-gpiochip", &error)) << error;
  EXPECT_TRUE(manager.Ready());
  EXPECT_EQ(recording->openedChipPath, "/dev/test-gpiochip");
  ASSERT_EQ(recording->openedValues.size(), 3u);
  EXPECT_EQ(recording->openedValues[0].offset, 256u);
  EXPECT_FALSE(recording->openedValues[0].value);
  EXPECT_EQ(recording->openedValues[1].offset, 39u);
  EXPECT_FALSE(recording->openedValues[1].value);
  EXPECT_EQ(recording->openedValues[2].offset, 257u);
  EXPECT_TRUE(recording->openedValues[2].value);

  EXPECT_TRUE(manager.Stop(&error)) << error;
}

// 验证：DO1 和 DO2 的 BOOL 命令分别写入 GPIO256 与 GPIO39，并回填完整接受响应。
TEST(BoardOutputManagerTest, MapsOutputCommandsAndReturnsAcceptedEndpoint) {
  auto driver = std::make_unique<RecordingGpioOutputDriver>();
  auto* recording = driver.get();
  BoardOutputManager manager(std::move(driver));
  std::string error;
  ASSERT_TRUE(manager.Start("/dev/test-gpiochip", &error)) << error;

  auto do1Request = MakeCommand("DO1", true);
  DataCenterProto::ExecuteCommandResponse response;
  ASSERT_TRUE(manager.ExecuteCommand(do1Request, 42, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_ACCEPTED);
  EXPECT_EQ(response.dst().module_name(), "BoardIO");
  EXPECT_EQ(response.dst().conn_name(), "board-do");
  EXPECT_EQ(response.dst().conn_id(), 42u);
  EXPECT_EQ(response.dst().tag(), "DO1");
  EXPECT_DOUBLE_EQ(response.requested_value(), 1.0);
  EXPECT_DOUBLE_EQ(response.accepted_value(), 1.0);

  auto do2Request = MakeCommand("DO2", false);
  response.Clear();
  ASSERT_TRUE(manager.ExecuteCommand(do2Request, 42, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_ACCEPTED);
  EXPECT_EQ(response.dst().tag(), "DO2");
  EXPECT_DOUBLE_EQ(response.requested_value(), 0.0);
  EXPECT_DOUBLE_EQ(response.accepted_value(), 0.0);

  ASSERT_EQ(recording->writes.size(), 2u);
  EXPECT_EQ(recording->writes[0].offset, 256u);
  EXPECT_TRUE(recording->writes[0].value);
  EXPECT_EQ(recording->writes[1].offset, 39u);
  EXPECT_FALSE(recording->writes[1].value);

  EXPECT_TRUE(manager.Stop(&error)) << error;
}

// 验证：DO 点拒绝非 BOOL 命令且不写 GPIO。
TEST(BoardOutputManagerTest, RejectsNonBoolCommand) {
  auto driver = std::make_unique<RecordingGpioOutputDriver>();
  auto* recording = driver.get();
  BoardOutputManager manager(std::move(driver));
  std::string error;
  ASSERT_TRUE(manager.Start("/dev/test-gpiochip", &error)) << error;

  auto request = MakeCommand("DO1", true);
  request.mutable_value()->set_int_value(1);
  DataCenterProto::ExecuteCommandResponse response;
  ASSERT_TRUE(manager.ExecuteCommand(request, 42, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_REJECTED);
  EXPECT_EQ(response.reject_code(), DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
  EXPECT_TRUE(recording->writes.empty());

  EXPECT_TRUE(manager.Stop(&error)) << error;
}

// 验证：未知 DO 点被明确拒绝且不写 GPIO。
TEST(BoardOutputManagerTest, RejectsUnknownOutputTag) {
  auto driver = std::make_unique<RecordingGpioOutputDriver>();
  auto* recording = driver.get();
  BoardOutputManager manager(std::move(driver));
  std::string error;
  ASSERT_TRUE(manager.Start("/dev/test-gpiochip", &error)) << error;

  auto request = MakeCommand("DO3", true);
  DataCenterProto::ExecuteCommandResponse response;
  ASSERT_TRUE(manager.ExecuteCommand(request, 42, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_REJECTED);
  EXPECT_EQ(response.reject_code(), DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
  EXPECT_TRUE(recording->writes.empty());

  EXPECT_TRUE(manager.Stop(&error)) << error;
}

// 验证：目的模块名、连接名或 conn_id 不匹配时均拒绝命令且不写 GPIO。
TEST(BoardOutputManagerTest, RejectsMismatchedDestinationConnection) {
  auto driver = std::make_unique<RecordingGpioOutputDriver>();
  auto* recording = driver.get();
  BoardOutputManager manager(std::move(driver));
  std::string error;
  ASSERT_TRUE(manager.Start("/dev/test-gpiochip", &error)) << error;

  std::vector<DataCenterProto::ExecuteCommandRequest> requests;
  requests.push_back(MakeCommand("DO1", true));
  requests.back().mutable_dst()->set_module_name("OtherModule");
  requests.push_back(MakeCommand("DO1", true));
  requests.back().mutable_dst()->set_conn_name("other-connection");
  requests.push_back(MakeCommand("DO1", true, 43));

  for (const auto& request : requests) {
    DataCenterProto::ExecuteCommandResponse response;
    ASSERT_TRUE(manager.ExecuteCommand(request, 42, &response).ok());
    EXPECT_EQ(response.status(), DataCenterProto::COMMAND_REJECTED);
  }
  EXPECT_TRUE(recording->writes.empty());

  EXPECT_TRUE(manager.Stop(&error)) << error;
}

// 验证：输出 GPIO 尚未启动时命令返回目标不可用且不尝试写入。
TEST(BoardOutputManagerTest, ReportsUnavailableBeforeOutputStarts) {
  auto driver = std::make_unique<RecordingGpioOutputDriver>();
  auto* recording = driver.get();
  BoardOutputManager manager(std::move(driver));

  auto request = MakeCommand("DO1", true);
  DataCenterProto::ExecuteCommandResponse response;
  ASSERT_TRUE(manager.ExecuteCommand(request, 42, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
  EXPECT_FALSE(manager.Ready());
  EXPECT_TRUE(recording->writes.empty());
}

// 验证：GPIO 写入失败时命令返回目标不可用，不能提前确认成功。
TEST(BoardOutputManagerTest, ReportsUnavailableWhenGpioWriteFails) {
  auto driver = std::make_unique<RecordingGpioOutputDriver>();
  auto* recording = driver.get();
  BoardOutputManager manager(std::move(driver));
  std::string error;
  ASSERT_TRUE(manager.Start("/dev/test-gpiochip", &error)) << error;
  recording->failWriteOffset = 256;

  auto request = MakeCommand("DO1", true);
  DataCenterProto::ExecuteCommandResponse response;
  ASSERT_TRUE(manager.ExecuteCommand(request, 42, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
  ASSERT_EQ(recording->writes.size(), 1u);
  EXPECT_EQ(recording->writes[0].offset, 256u);
  EXPECT_TRUE(recording->writes[0].value);

  recording->failWriteOffset.reset();
  EXPECT_TRUE(manager.Stop(&error)) << error;
}

// 验证：命令等待输出锁期间被取消后原样返回取消状态，且不得继续写入 GPIO。
TEST(BoardOutputManagerTest, DoesNotWriteAfterCancellationWhileWaitingForOutputLock) {
  auto driver = std::make_unique<RecordingGpioOutputDriver>();
  auto* recording = driver.get();
  BoardOutputManager manager(std::move(driver));
  std::string error;
  ASSERT_TRUE(manager.Start("/dev/test-gpiochip", &error)) << error;
  recording->BlockNextSetValue();

  auto firstRequest = MakeCommand("DO1", true);
  DataCenterProto::ExecuteCommandResponse firstResponse;
  grpc::Status firstStatus;
  std::thread firstCommand([&]() {
    firstStatus = manager.ExecuteCommand(firstRequest, 42, &firstResponse);
  });
  if (!recording->WaitUntilSetValueBlocked()) {
    recording->ReleaseBlockedSetValue();
    firstCommand.join();
    FAIL() << "首条命令未按预期阻塞在 GPIO 写入";
  }

  std::atomic<bool> cancelled = false;
  std::atomic<int> cancellationChecks = 0;
  std::promise<void> initialCheckPromise;
  auto initialCheck = initialCheckPromise.get_future();
  auto secondRequest = MakeCommand("DO2", true);
  DataCenterProto::ExecuteCommandResponse secondResponse;
  grpc::Status secondStatus;
  std::thread secondCommand([&]() {
    secondStatus = manager.ExecuteCommand(
        secondRequest, 42, &secondResponse, [&]() {
          const int checkNumber = cancellationChecks.fetch_add(1) + 1;
          if (checkNumber == 1) {
            initialCheckPromise.set_value();
          }
          if (cancelled.load()) {
            return grpc::Status(grpc::StatusCode::CANCELLED, "模拟命令取消");
          }
          return grpc::Status::OK;
        });
  });

  const auto initialCheckResult = initialCheck.wait_for(std::chrono::seconds(1));
  cancelled.store(true);
  recording->ReleaseBlockedSetValue();
  firstCommand.join();
  secondCommand.join();

  ASSERT_EQ(initialCheckResult, std::future_status::ready);
  EXPECT_TRUE(firstStatus.ok());
  EXPECT_EQ(firstResponse.status(), DataCenterProto::COMMAND_ACCEPTED);
  EXPECT_EQ(secondStatus.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_EQ(secondStatus.error_message(), "模拟命令取消");
  EXPECT_GE(cancellationChecks.load(), 1);
  ASSERT_EQ(recording->writes.size(), 1u);
  EXPECT_EQ(recording->writes[0].offset, 256u);

  EXPECT_TRUE(manager.Stop(&error)) << error;
}

// 验证：停止模块内输出功能时依次将 DO1、DO2 和失电继电器拉低，再释放 GPIO。
TEST(BoardOutputManagerTest, PullsAllOutputsLowBeforeClosing) {
  auto driver = std::make_unique<RecordingGpioOutputDriver>();
  auto* recording = driver.get();
  BoardOutputManager manager(std::move(driver));
  std::string error;
  ASSERT_TRUE(manager.Start("/dev/test-gpiochip", &error)) << error;
  recording->writes.clear();

  ASSERT_TRUE(manager.Stop(&error)) << error;
  EXPECT_FALSE(manager.Ready());
  EXPECT_TRUE(recording->closed);
  ASSERT_EQ(recording->writes.size(), 3u);
  EXPECT_EQ(recording->writes[0].offset, 256u);
  EXPECT_FALSE(recording->writes[0].value);
  EXPECT_EQ(recording->writes[1].offset, 39u);
  EXPECT_FALSE(recording->writes[1].value);
  EXPECT_EQ(recording->writes[2].offset, 257u);
  EXPECT_FALSE(recording->writes[2].value);
}

}  // namespace
}  // namespace BoardIO
