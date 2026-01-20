#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ConfigPusherModuleManager.h"
#include "Logger.h"

namespace {
void InitLoggerOnce() {
  static bool inited = false;
  if (!inited) {
    ModuleManager::Logger::init("./log", "config_pusher_module_manager_test.log");
    inited = true;
  }
}

class FakeModuleManagerStub final : public ModuleManagerProto::ModuleManage::StubInterface {
public:
  grpc::Status GetModuleInfo(grpc::ClientContext*, const ModuleManagerProto::Empty&,
                             ModuleManagerProto::ModuleInfos* response) override {
    if (!getModuleInfoStatus_.ok()) {
      return getModuleInfoStatus_;
    }
    if (response != nullptr) {
      *response = moduleInfos_;
    }
    return grpc::Status::OK;
  }

  grpc::Status GetRunningModuleInfo(grpc::ClientContext*, const ModuleManagerProto::Empty&,
                                    ModuleManagerProto::ModuleRunningInfos* response) override {
    if (!getRunningInfoStatus_.ok()) {
      return getRunningInfoStatus_;
    }
    if (response != nullptr) {
      if (runningInfosSequence_.empty()) {
        response->Clear();
      } else {
        const auto index = std::min(runningInfosIndex_, runningInfosSequence_.size() - 1);
        *response = runningInfosSequence_[index];
        if (runningInfosIndex_ < runningInfosSequence_.size()) {
          ++runningInfosIndex_;
        }
      }
    }
    return grpc::Status::OK;
  }

  grpc::Status StartModule(grpc::ClientContext*, const ModuleManagerProto::ModuleInfo&,
                           ModuleManagerProto::Empty*) override {
    return startModuleStatus_;
  }

  grpc::Status StopModule(grpc::ClientContext*, const ModuleManagerProto::ModuleInfo&,
                          ModuleManagerProto::Empty*) override {
    return grpc::Status::OK;
  }

  grpc::Status UploadModule(grpc::ClientContext*, const ModuleManagerProto::Empty&,
                            ModuleManagerProto::Empty*) override {
    return grpc::Status::OK;
  }

  grpc::Status DeleteModule(grpc::ClientContext*, const ModuleManagerProto::ModuleInfo&,
                            ModuleManagerProto::Empty*) override {
    return grpc::Status::OK;
  }

  grpc::Status SaveModuleStartConfig(grpc::ClientContext*, const ModuleManagerProto::ModuleInfos&,
                                     ModuleManagerProto::Empty*) override {
    return grpc::Status::OK;
  }

  void SetModuleInfos(ModuleManagerProto::ModuleInfos infos) { moduleInfos_ = std::move(infos); }

  void SetRunningInfosSequence(std::vector<ModuleManagerProto::ModuleRunningInfos> seq) {
    runningInfosSequence_ = std::move(seq);
    runningInfosIndex_ = 0;
  }

  void SetGetModuleInfoStatus(grpc::Status status) { getModuleInfoStatus_ = std::move(status); }

  void SetGetRunningInfoStatus(grpc::Status status) { getRunningInfoStatus_ = std::move(status); }

  void SetStartModuleStatus(grpc::Status status) { startModuleStatus_ = std::move(status); }

private:
  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::ModuleInfos>* AsyncGetModuleInfoRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::Empty& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::ModuleInfos>* PrepareAsyncGetModuleInfoRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::Empty& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::ModuleRunningInfos>* AsyncGetRunningModuleInfoRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::Empty& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::ModuleRunningInfos>* PrepareAsyncGetRunningModuleInfoRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::Empty& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::Empty>* AsyncStartModuleRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::ModuleInfo& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::Empty>* PrepareAsyncStartModuleRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::ModuleInfo& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::Empty>* AsyncStopModuleRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::ModuleInfo& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::Empty>* PrepareAsyncStopModuleRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::ModuleInfo& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::Empty>* AsyncUploadModuleRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::Empty& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::Empty>* PrepareAsyncUploadModuleRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::Empty& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::Empty>* AsyncDeleteModuleRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::ModuleInfo& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::Empty>* PrepareAsyncDeleteModuleRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::ModuleInfo& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::Empty>* AsyncSaveModuleStartConfigRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::ModuleInfos& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncResponseReaderInterface<::ModuleManagerProto::Empty>* PrepareAsyncSaveModuleStartConfigRaw(
      ::grpc::ClientContext* /*context*/,
      const ::ModuleManagerProto::ModuleInfos& /*request*/,
      ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  ModuleManagerProto::ModuleInfos moduleInfos_;
  std::vector<ModuleManagerProto::ModuleRunningInfos> runningInfosSequence_;
  size_t runningInfosIndex_ = 0;
  grpc::Status getModuleInfoStatus_ = grpc::Status::OK;
  grpc::Status getRunningInfoStatus_ = grpc::Status::OK;
  grpc::Status startModuleStatus_ = grpc::Status::OK;
};
}  // namespace

// 验证：获取模块信息成功时返回列表内容。
TEST(ConfigPusherModuleManagerTest, FetchModuleInfosSuccess) {
  InitLoggerOnce();
  FakeModuleManagerStub stub;

  ModuleManagerProto::ModuleInfos infos;
  auto* info = infos.add_module_info();
  info->set_module_name("IEC104");
  info->set_lib_name("libIEC104.so");
  stub.SetModuleInfos(std::move(infos));

  ModuleManagerProto::ModuleInfos out;
  EXPECT_TRUE(ConfigPusher::fetchModuleInfos(&stub, &out));
  ASSERT_EQ(out.module_info_size(), 1);
  EXPECT_EQ(out.module_info(0).module_name(), "IEC104");
}

// 验证：获取运行中模块信息失败时返回 false。
TEST(ConfigPusherModuleManagerTest, FetchRunningModuleInfosFailure) {
  InitLoggerOnce();
  FakeModuleManagerStub stub;
  stub.SetGetRunningInfoStatus(grpc::Status(grpc::StatusCode::UNAVAILABLE, "服务不可用"));

  ModuleManagerProto::ModuleRunningInfos out;
  EXPECT_FALSE(ConfigPusher::fetchRunningModuleInfos(&stub, &out));
}

// 验证：启动模块失败时返回 false。
TEST(ConfigPusherModuleManagerTest, StartModuleFailure) {
  InitLoggerOnce();
  FakeModuleManagerStub stub;
  stub.SetStartModuleStatus(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "启动失败"));

  ModuleManagerProto::ModuleInfo info;
  info.set_module_name("IEC104");

  EXPECT_FALSE(ConfigPusher::startModule(&stub, info));
}

// 验证：等待模块运行时命中运行中列表可直接返回。
TEST(ConfigPusherModuleManagerTest, WaitForModuleReturnsRunningInfo) {
  InitLoggerOnce();
  FakeModuleManagerStub stub;

  ModuleManagerProto::ModuleRunningInfos runningInfos;
  auto* running = runningInfos.add_module_running_info();
  running->set_module_name("IEC104");
  running->set_inner_grpc_server("unix://socket/IEC104.sock");
  stub.SetRunningInfosSequence({runningInfos});

  auto result = ConfigPusher::waitForModule(&stub, "IEC104", std::chrono::milliseconds(10));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->module_name(), "IEC104");
}
