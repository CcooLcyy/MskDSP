#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "ModuleManagerGrpcService.h"
#include "PlatformAccessSupervisor.h"

using namespace std::chrono_literals;
namespace pa = mskdsp::platformagent;
namespace {
class PlatformAccessTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto pattern = (std::filesystem::current_path() / "mskdsp-access-XXXXXX").string();
    const auto created = ::mkdtemp(pattern.data());
    ASSERT_NE(created, nullptr);
    directory = created;
    path = directory / "platform_access.json";
  }
  void TearDown() override {
    std::filesystem::remove_all(directory);
  }
  static ModuleManagerProto::SetPlatformAccessConfigRequest Request(uint64_t revision = 0) {
    ModuleManagerProto::SetPlatformAccessConfigRequest request;
    request.set_expected_revision(revision);
    request.set_enabled(true);
    request.set_device_id("test-device.1");
    request.set_device_token("test-token");
    return request;
  }
  static ModuleManagerProto::PlatformAccessStatus Read(pa::PlatformAccessSupervisor &control) {
    ModuleManagerProto::PlatformAccessStatus status;
    EXPECT_TRUE(control.Get(&status).ok());
    return status;
  }
  std::filesystem::path directory, path;
};

// 未配置时禁用且读取不创建文件，保存只返回令牌标记并以0600落盘，重建恢复相同修订。
TEST_F(PlatformAccessTest, DefaultsAndPersistsRedactedConfiguration) {
  pa::PlatformAccessSupervisor control(path);
  auto initial = Read(control);
  EXPECT_FALSE(initial.enabled());
  EXPECT_EQ(initial.revision(), 0);
  EXPECT_EQ(initial.platform_target(), "60.208.15.90:50000");
  EXPECT_FALSE(std::filesystem::exists(path));
  auto request = Request();
  ModuleManagerProto::PlatformAccessStatus saved;
  ASSERT_TRUE(control.Set(request, &saved).ok());
  EXPECT_TRUE(saved.token_configured());
  EXPECT_EQ(saved.ssh_port(), 22);
  EXPECT_FALSE(saved.online());
  EXPECT_EQ(saved.revision(), 1);
  EXPECT_EQ(saved.SerializeAsString().find("test-token"), std::string::npos);
  struct stat info{};
  ASSERT_EQ(::stat(path.c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 0777, 0600);
  pa::PlatformAccessSupervisor restored(path);
  EXPECT_EQ(Read(restored).SerializeAsString(), saved.SerializeAsString());
}

// 过期修订不能写入，空令牌保留，相同内容幂等，更换身份禁止沿用旧令牌。
TEST_F(PlatformAccessTest, EnforcesRevisionAndIdentityTokenRules) {
  pa::PlatformAccessSupervisor control(path);
  auto request = Request();
  ModuleManagerProto::PlatformAccessStatus saved;
  ASSERT_TRUE(control.Set(request, &saved).ok());
  EXPECT_EQ(control.Set(request, &saved).error_code(), grpc::StatusCode::ABORTED);
  request.set_expected_revision(1);
  request.clear_device_token();
  ASSERT_TRUE(control.Set(request, &saved).ok());
  EXPECT_EQ(saved.revision(), 1);
  request.set_device_id("another-device");
  EXPECT_EQ(control.Set(request, &saved).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request.set_enabled(false);
  request.set_clear_device_token(true);
  ASSERT_TRUE(control.Set(request, &saved).ok());
  EXPECT_FALSE(saved.token_configured());
  EXPECT_EQ(saved.revision(), 2);
  request.set_expected_revision(2);
  request.set_enabled(true);
  EXPECT_EQ(control.Set(request, &saved).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 非法身份、令牌、端口以及互斥清除均不产生持久文件。
TEST_F(PlatformAccessTest, RejectsInvalidInputWithoutWriting) {
  pa::PlatformAccessSupervisor control(path);
  ModuleManagerProto::PlatformAccessStatus saved;
  auto invalid = [&](auto modify) {
    auto request = Request();
    modify(request);
    EXPECT_EQ(control.Set(request, &saved).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_FALSE(std::filesystem::exists(path));
  };
  invalid([](auto &r) { r.set_device_id("bad/id"); });
  invalid([](auto &r) { r.set_device_id(std::string(129, 'a')); });
  invalid([](auto &r) { r.set_device_token("token with space"); });
  invalid([](auto &r) { r.set_device_token(std::string(4097, 'a')); });
  invalid([](auto &r) { r.set_clear_device_token(true); });
  invalid([](auto &r) { r.set_ssh_port(65536); });
}

// 损坏或符号链接文件保持禁用并返回脱敏错误，不修改链接指向的内容。
TEST_F(PlatformAccessTest, RejectsCorruptionAndSymlinks) {
  std::ofstream(path) << "{test-secret-corrupt";
  ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
  pa::PlatformAccessSupervisor corrupt(path);
  auto status = Read(corrupt);
  EXPECT_FALSE(status.enabled());
  EXPECT_FALSE(status.last_error().empty());
  EXPECT_EQ(status.last_error().find("test-secret"), std::string::npos);
  const auto other = directory / "other.json";
  std::filesystem::rename(path, other);
  std::filesystem::create_symlink(other, path);
  pa::PlatformAccessSupervisor symlink(path);
  EXPECT_FALSE(Read(symlink).last_error().empty());
  auto request = Request();
  EXPECT_EQ(symlink.Set(request, &status).error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_TRUE(std::filesystem::is_symlink(path));
  std::ifstream input(other);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "{test-secret-corrupt");
}

// 目录缺失导致保存失败，配置和修订不变，不能启动连接任务。
TEST_F(PlatformAccessTest, FailedSaveKeepsPreviousConfiguration) {
  pa::PlatformAccessSupervisor control(directory / "missing" / "platform_access.json");
  auto before = Read(control);
  ModuleManagerProto::PlatformAccessStatus status;
  EXPECT_EQ(control.Set(Request(), &status).error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(Read(control).revision(), before.revision());
  EXPECT_FALSE(Read(control).enabled());
}

struct FakeRunner {
  std::mutex mutex;
  std::condition_variable_any event;
  int starts{}, stops{};
  std::vector<pa::PlatformStatusObserver> observers;
  std::vector<std::string> identities;
  bool Wait(int expectedStarts, int expectedStops) {
    std::unique_lock lock(mutex);
    return event.wait_for(lock, 5s, [&] { return starts == expectedStarts && stops == expectedStops; });
  }
  void operator()(const pa::Config &config, std::stop_token stop, pa::PlatformStatusObserver observer) {
    std::unique_lock lock(mutex);
    ++starts;
    observers.push_back(observer);
    identities.push_back(config.deviceId);
    EXPECT_EQ(config.target, "60.208.15.90:50000");
    event.notify_all();
    event.wait(lock, stop, [] { return false; });
    ++stops;
    event.notify_all();
  }
};

// 热切换先取消旧任务，禁用后不再运行，旧代次上线回调不能污染新配置状态。
TEST_F(PlatformAccessTest, ReconfiguresAndIgnoresOldWorkerCallbacks) {
  FakeRunner runner;
  pa::PlatformAccessSupervisor control(path, std::ref(runner));
  std::jthread background([&](std::stop_token stop) { control.Run(stop); });
  ModuleManagerProto::PlatformAccessStatus saved;
  auto request = Request();
  ASSERT_TRUE(control.Set(request, &saved).ok());
  ASSERT_TRUE(runner.Wait(1, 0));
  request.set_expected_revision(1);
  request.clear_device_token();
  ASSERT_TRUE(control.Set(request, &saved).ok());
  EXPECT_EQ(saved.revision(), 1);
  EXPECT_TRUE(runner.Wait(1, 0));
  pa::PlatformStatusObserver old;
  {
    std::lock_guard lock(runner.mutex);
    old = runner.observers.front();
  }
  old(true, "");
  EXPECT_TRUE(Read(control).online());
  request.set_expected_revision(1);
  request.set_device_token("replacement-token");
  ASSERT_TRUE(control.Set(request, &saved).ok());
  EXPECT_FALSE(saved.online());
  ASSERT_TRUE(runner.Wait(2, 1));
  old(true, "旧连接错误");
  EXPECT_FALSE(Read(control).online());
  EXPECT_TRUE(Read(control).last_error().empty());
  request.set_expected_revision(2);
  request.set_enabled(false);
  request.clear_device_token();
  ASSERT_TRUE(control.Set(request, &saved).ok());
  ASSERT_TRUE(runner.Wait(2, 2));
  request.set_expected_revision(3);
  request.set_enabled(true);
  ASSERT_TRUE(control.Set(request, &saved).ok());
  ASSERT_TRUE(runner.Wait(3, 2));
  background.request_stop();
  background.join();
  EXPECT_TRUE(runner.Wait(3, 3));
}

// 重建后读取已保存身份并启动连接任务，不依赖环境变量或重新下发。
TEST_F(PlatformAccessTest, RestoresAndRunsPersistedIdentity) {
  {
    pa::PlatformAccessSupervisor original(path);
    ModuleManagerProto::PlatformAccessStatus saved;
    ASSERT_TRUE(original.Set(Request(), &saved).ok());
  }
  FakeRunner runner;
  pa::PlatformAccessSupervisor restored(path, std::ref(runner));
  std::jthread background([&](std::stop_token stop) { restored.Run(stop); });
  ASSERT_TRUE(runner.Wait(1, 0));
  EXPECT_EQ(Read(restored).device_id(), "test-device.1");
  {
    std::lock_guard lock(runner.mutex);
    EXPECT_EQ(runner.identities.front(), "test-device.1");
  }
  EXPECT_TRUE(Read(restored).enabled());
  background.request_stop();
  background.join();
  EXPECT_TRUE(runner.Wait(1, 1));
}

// 热保存落盘失败时不取消旧连接任务，也不更改修订或身份。
TEST_F(PlatformAccessTest, FailedHotSaveDoesNotSwitchWorker) {
  const auto conf = directory / "conf";
  std::filesystem::create_directory(conf);
  FakeRunner runner;
  pa::PlatformAccessSupervisor control(conf / "platform_access.json", std::ref(runner));
  std::jthread background([&](std::stop_token stop) { control.Run(stop); });
  ModuleManagerProto::PlatformAccessStatus status;
  ASSERT_TRUE(control.Set(Request(), &status).ok());
  ASSERT_TRUE(runner.Wait(1, 0));
  std::filesystem::rename(conf, directory / "moved");
  auto request = Request(1);
  request.set_device_id("replacement-device");
  EXPECT_EQ(control.Set(request, &status).error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(Read(control).revision(), 1);
  EXPECT_EQ(Read(control).device_id(), "test-device.1");
  EXPECT_TRUE(runner.Wait(1, 0));
  background.request_stop();
  background.join();
  EXPECT_TRUE(runner.Wait(1, 1));
}

// 两个调用者使用相同修订并发保存，只允许一个成功，重建后仍为胜者的配置。
TEST_F(PlatformAccessTest, SerializesConcurrentRevisionUpdates) {
  pa::PlatformAccessSupervisor control(path);
  auto first = Request();
  auto second = Request();
  second.set_device_id("second-device");
  grpc::Status a, b;
  ModuleManagerProto::PlatformAccessStatus sa, sb;
  std::jthread one([&] { a = control.Set(first, &sa); });
  std::jthread two([&] { b = control.Set(second, &sb); });
  one.join();
  two.join();
  EXPECT_NE(a.ok(), b.ok());
  EXPECT_EQ((a.ok() ? b : a).error_code(), grpc::StatusCode::ABORTED);
  pa::PlatformAccessSupervisor restored(path);
  EXPECT_EQ(Read(restored).revision(), 1);
  EXPECT_EQ(Read(restored).device_id(), a.ok() ? "test-device.1" : "second-device");
}

// 已保存文件一旦被放宽权限即拒绝加载，避免默认启用可被其他账户读取的凭据。
TEST_F(PlatformAccessTest, RejectsCredentialFileWithPublicPermissions) {
  pa::PlatformAccessSupervisor original(path);
  ModuleManagerProto::PlatformAccessStatus saved;
  ASSERT_TRUE(original.Set(Request(), &saved).ok());
  ASSERT_EQ(::chmod(path.c_str(), 0644), 0);
  pa::PlatformAccessSupervisor restored(path);
  EXPECT_FALSE(Read(restored).enabled());
  EXPECT_FALSE(Read(restored).last_error().empty());
}

// 连接任务意外退出后，相同配置重新保存也能恢复；普通连接重试无需重新配置。
TEST_F(PlatformAccessTest, SameConfigurationRecoversFailedRunner) {
  FakeRunner runner;
  std::atomic_int attempts{0};
  pa::PlatformAccessSupervisor control(path, [&](const pa::Config &config, std::stop_token stop, pa::PlatformStatusObserver observer) {
    if (attempts.fetch_add(1) == 0)
      throw std::runtime_error("不应输出的内部异常");
    runner(config, stop, std::move(observer));
  });
  std::jthread background([&](std::stop_token stop) { control.Run(stop); });
  ModuleManagerProto::PlatformAccessStatus status;
  auto request = Request();
  ASSERT_TRUE(control.Set(request, &status).ok());
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (Read(control).last_error().empty() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_FALSE(Read(control).last_error().empty());
  EXPECT_EQ(Read(control).last_error().find("不应输出"), std::string::npos);
  request.set_expected_revision(1);
  request.clear_device_token();
  ASSERT_TRUE(control.Set(request, &status).ok());
  ASSERT_TRUE(runner.Wait(1, 0));
  EXPECT_TRUE(Read(control).last_error().empty());
  background.request_stop();
  background.join();
  EXPECT_EQ(attempts.load(), 2);
}

// 真正的管理gRPC拒绝未装控制器，注入后脱敏读取并持久保存，旧修订错误保持一致。
TEST_F(PlatformAccessTest, ServesManagementRpcOnRandomLocalPort) {
  ModuleManager::ModuleManagerServiceImpl service;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  auto stub = ModuleManagerProto::ModuleManage::NewStub(grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  auto get = [&] {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    ModuleManagerProto::Empty request;
    ModuleManagerProto::PlatformAccessStatus response;
    return stub->GetPlatformAccessConfig(&context, request, &response);
  };
  EXPECT_EQ(get().error_code(), grpc::StatusCode::UNAVAILABLE);
  auto control = std::make_shared<pa::PlatformAccessSupervisor>(path);
  service.SetPlatformAccessControl(control);
  EXPECT_TRUE(get().ok());
  for (int attempt = 0; attempt < 2; ++attempt) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    ModuleManagerProto::PlatformAccessStatus response;
    const auto status = stub->SetPlatformAccessConfig(&context, Request(), &response);
    EXPECT_EQ(status.error_code(), attempt == 0 ? grpc::StatusCode::OK : grpc::StatusCode::ABORTED);
    EXPECT_EQ(response.SerializeAsString().find("test-token"), std::string::npos);
  }
  server->Shutdown(std::chrono::system_clock::now() + 2s);
}
}  // namespace
