#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// 架构别名统一为协议值，未知架构保留原值，空探测结果不得猜测为 ARM64。
TEST_F(PlatformAccessTest, NormalizesDeviceArchitectureWithoutGuessingUnknownValues) {
  for (const auto &alias : {"aarch64", "arm64", "AARCH64"})
    EXPECT_EQ(pa::NormalizePlatformDeviceArchitecture(alias), "arm64");
  for (const auto &alias : {"x86_64", "amd64", "x64", "AMD64"})
    EXPECT_EQ(pa::NormalizePlatformDeviceArchitecture(alias), "x64");
  EXPECT_EQ(pa::NormalizePlatformDeviceArchitecture("riscv64"), "riscv64");
  EXPECT_EQ(pa::NormalizePlatformDeviceArchitecture("CustomCPU"), "CustomCPU");
  EXPECT_EQ(pa::NormalizePlatformDeviceArchitecture(""), "unknown");
}

// Get 和 Set 均报告下位机运行环境 uname 架构，不依赖保存配置或其他模块。
TEST_F(PlatformAccessTest, ReportsRuntimeDeviceArchitectureInReadAndSaveStatus) {
  struct utsname runtime{};
  ASSERT_EQ(::uname(&runtime), 0);
  const auto expected = pa::NormalizePlatformDeviceArchitecture(runtime.machine);
  pa::PlatformAccessSupervisor control(path);
  EXPECT_EQ(Read(control).device_architecture(), expected);
  EXPECT_FALSE(std::filesystem::exists(path));
  ModuleManagerProto::PlatformAccessStatus saved;
  ASSERT_TRUE(control.Set(Request(), &saved).ok());
  EXPECT_EQ(saved.device_architecture(), expected);
  EXPECT_EQ(Read(control).device_architecture(), saved.device_architecture());
}

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
// 平台关闭时仍应用普通出口，旧请求省略网络消息时保留配置，并能从持久文件恢复。
TEST_F(PlatformAccessTest, NetworkConfigurationIsIndependentAndBackwardCompatible) {
  std::vector<pa::OrdinaryNetworkConfig> applied;
  auto apply = [&](const pa::OrdinaryNetworkConfig &network) {
    applied.push_back(network);
    return pa::OrdinaryNetworkResult{true, "", "192.168.2.219"};
  };
  pa::PlatformAccessSupervisor control(path, {}, apply, [](const pa::OrdinaryNetworkConfig &) { return pa::OrdinaryNetworkResult{true, "", "192.168.2.219"}; });
  auto request = Request();
  request.set_enabled(false);
  auto *network = request.mutable_network_config();
  network->set_interface_name("eth0.102");
  network->set_gateway("192.168.2.1");
  network->add_dns_servers("192.168.2.1");
  ModuleManagerProto::PlatformAccessStatus saved;
  ASSERT_TRUE(control.Set(request, &saved).ok());
  EXPECT_FALSE(saved.enabled());
  EXPECT_TRUE(saved.network_applied());
  EXPECT_EQ(saved.network_source_address(), "192.168.2.219");
  ASSERT_EQ(applied.size(), 1);
  request = Request(1);
  request.set_enabled(false);
  request.clear_device_token();
  ASSERT_TRUE(control.Set(request, &saved).ok());
  EXPECT_EQ(saved.revision(), 1);
  EXPECT_EQ(saved.network_config().gateway(), "192.168.2.1");
  EXPECT_EQ(applied.size(), 1);
  std::atomic_bool restoredReady{false};
  pa::PlatformAccessSupervisor restored(path, {}, [&](const pa::OrdinaryNetworkConfig &network) {
    restoredReady = true;
    return apply(network); }, [&](const pa::OrdinaryNetworkConfig &) { return pa::OrdinaryNetworkResult{restoredReady, restoredReady ? "" : "尚未恢复", "192.168.2.219"}; });
  EXPECT_EQ(Read(restored).network_config().interface_name(), "eth0.102");
  EXPECT_FALSE(Read(restored).network_applied());
  std::jthread background([&](std::stop_token stop) { restored.Run(stop); });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!Read(restored).network_applied() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  EXPECT_TRUE(Read(restored).network_applied());
  EXPECT_FALSE(Read(restored).enabled());
  background.request_stop();
  background.join();
}

// 网络应用失败必须回滚且不增加持久修订，后续用相同修订可以重试。
TEST_F(PlatformAccessTest, NetworkFailureRollsBackAndCanRetry) {
  int attempts = 0;
  pa::PlatformAccessSupervisor control(path, {}, [&](const pa::OrdinaryNetworkConfig &network) {
    if (!network.interfaceName.empty() && ++attempts == 1)
      return pa::OrdinaryNetworkResult{false, "网络命令失败", ""};
    return pa::OrdinaryNetworkResult{true, "", ""};
  });
  auto request = Request();
  request.mutable_network_config()->set_interface_name("eth0.102");
  request.mutable_network_config()->set_gateway("192.168.2.1");
  ModuleManagerProto::PlatformAccessStatus saved;
  EXPECT_EQ(control.Set(request, &saved).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(Read(control).revision(), 0);
  EXPECT_FALSE(saved.network_error().empty());
  EXPECT_FALSE(std::filesystem::exists(path));
  ASSERT_TRUE(control.Set(request, &saved).ok());
  EXPECT_TRUE(saved.network_error().empty());
  EXPECT_EQ(saved.revision(), 1);
}

// 已应用网络后的保存失败会恢复旧网络；回滚失败单独报告，不能显示应用成功。
TEST_F(PlatformAccessTest, FailedNetworkSaveReportsRollbackFailure) {
  int calls = 0;
  pa::PlatformAccessSupervisor control(directory / "missing" / "platform_access.json", {}, [&](const pa::OrdinaryNetworkConfig &) {
    ++calls;
    return calls == 1 ? pa::OrdinaryNetworkResult{true, "", "192.168.2.219"}
                      : pa::OrdinaryNetworkResult{false, "恢复失败", ""}; }, [](const pa::OrdinaryNetworkConfig &) { return pa::OrdinaryNetworkResult{false, "恢复失败", ""}; });
  auto request = Request();
  request.mutable_network_config()->set_interface_name("eth0.102");
  request.mutable_network_config()->set_gateway("192.168.2.1");
  ModuleManagerProto::PlatformAccessStatus saved;
  EXPECT_EQ(control.Set(request, &saved).error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(Read(control).revision(), 0);
  EXPECT_FALSE(Read(control).network_applied());
  EXPECT_NE(Read(control).network_error().find("恢复失败"), std::string::npos);
  EXPECT_EQ(calls, 2);
}

// 非法网络参数应在执行任何系统命令和写文件之前拒绝。
TEST_F(PlatformAccessTest, RejectsInvalidNetworkBeforeApplying) {
  int calls = 0;
  pa::PlatformAccessSupervisor control(path, {}, [&](const pa::OrdinaryNetworkConfig &) {
    ++calls;
    return pa::OrdinaryNetworkResult{true, "", ""};
  });
  auto request = Request();
  request.mutable_network_config()->set_interface_name("eth0.102;reboot");
  request.mutable_network_config()->set_gateway("192.168.2.1");
  ModuleManagerProto::PlatformAccessStatus status;
  EXPECT_EQ(control.Set(request, &status).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(calls, 0);
  EXPECT_FALSE(std::filesystem::exists(path));
}

// 旧版文件缺少网络字段时保留系统现有网络，读取时不执行任何网络修改。
TEST_F(PlatformAccessTest, LoadsLegacyFileWithoutNetworkFields) {
  std::ofstream(path) << R"({"schema_version":1,"revision":7,"enabled":false,"device_id":"legacy","device_token":"","ssh_enabled":false,"ssh_port":22})";
  ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
  int applied = 0;
  pa::PlatformAccessSupervisor restored(path, {}, [&](const pa::OrdinaryNetworkConfig &) {
    ++applied;
    return pa::OrdinaryNetworkResult{true, "", ""};
  });
  auto status = Read(restored);
  EXPECT_EQ(status.revision(), 7);
  EXPECT_TRUE(status.has_network_config());
  EXPECT_TRUE(status.network_config().interface_name().empty());
  EXPECT_EQ(applied, 0);
}

// 显式空网络消息恢复系统网络，省略消息的兼容语义不得误当成撤销。
TEST_F(PlatformAccessTest, ExplicitEmptyNetworkRestoresSystemNetwork) {
  std::vector<std::string> interfaces;
  pa::PlatformAccessSupervisor control(path, {}, [&](const pa::OrdinaryNetworkConfig &network) {
    interfaces.push_back(network.interfaceName);
    return pa::OrdinaryNetworkResult{true, "", ""};
  });
  auto request = Request();
  request.mutable_network_config()->set_interface_name("eth0.102");
  request.mutable_network_config()->set_gateway("192.168.2.1");
  ModuleManagerProto::PlatformAccessStatus status;
  ASSERT_TRUE(control.Set(request, &status).ok());
  request = Request(1);
  request.clear_device_token();
  request.mutable_network_config();
  ASSERT_TRUE(control.Set(request, &status).ok());
  ASSERT_EQ(interfaces.size(), 2);
  EXPECT_TRUE(interfaces.back().empty());
  EXPECT_TRUE(status.network_config().interface_name().empty());
  EXPECT_EQ(status.revision(), 2);
}

// 启动网络恢复失败时不能启动平台连接且独立报告网络错误。
TEST_F(PlatformAccessTest, StartupNetworkFailureDoesNotStartPlatform) {
  pa::StoredPlatformAccess stored;
  stored.revision = 1;
  stored.config.enabled = true;
  stored.config.deviceId = "configured";
  stored.config.token = "secret";
  stored.config.networkConfig.interfaceName = "eth0.102";
  stored.config.networkConfig.gateway = "192.168.2.1";
  ASSERT_EQ(pa::PlatformAccessStore(path).Save(stored), pa::PlatformAccessSave::Durable);
  std::atomic_int attempts{0}, starts{0};
  pa::PlatformAccessSupervisor control(path, [&](const pa::Config &, std::stop_token, pa::PlatformStatusObserver) { ++starts; }, [&](const pa::OrdinaryNetworkConfig &) {
        ++attempts;
        return pa::OrdinaryNetworkResult{false, "宿主机网络服务不可用", ""}; }, [](const pa::OrdinaryNetworkConfig &) { return pa::OrdinaryNetworkResult{false, "宿主机网络服务不可用", ""}; });
  std::jthread background([&](std::stop_token stop) { control.Run(stop); });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while ((attempts.load() == 0 || Read(control).network_error() != "宿主机网络服务不可用") && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  EXPECT_EQ(attempts.load(), 1);
  EXPECT_EQ(starts.load(), 0);
  EXPECT_FALSE(Read(control).network_applied());
  EXPECT_EQ(Read(control).network_error(), "宿主机网络服务不可用");
  EXPECT_TRUE(Read(control).last_error().empty());
  background.request_stop();
  background.join();
}

// 冷启动网口稍后就绪时自动重试恢复，即使平台关闭也无需用户再次保存。
TEST_F(PlatformAccessTest, RetriesStartupNetworkUntilInterfaceReady) {
  pa::StoredPlatformAccess stored;
  stored.revision = 1;
  stored.config.networkConfig.interfaceName = "eth0.102";
  stored.config.networkConfig.gateway = "192.168.2.1";
  ASSERT_EQ(pa::PlatformAccessStore(path).Save(stored), pa::PlatformAccessSave::Durable);
  std::atomic_int attempts{0};
  pa::PlatformAccessSupervisor control(path, {}, [&](const pa::OrdinaryNetworkConfig &) {
    if (++attempts == 1)
      return pa::OrdinaryNetworkResult{false, "网口尚未就绪", ""};
    return pa::OrdinaryNetworkResult{true, "", "192.168.2.219"}; }, [&](const pa::OrdinaryNetworkConfig &) { return pa::OrdinaryNetworkResult{attempts.load() >= 2, attempts.load() >= 2 ? "" : "网口尚未就绪", "192.168.2.219"}; });
  std::jthread background([&](std::stop_token stop) { control.Run(stop); });
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  while (!Read(control).network_applied() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  EXPECT_EQ(attempts.load(), 2);
  EXPECT_TRUE(Read(control).network_applied());
  EXPECT_TRUE(Read(control).network_error().empty());
  EXPECT_FALSE(Read(control).enabled());
  background.request_stop();
  background.join();
}
// 实际规则被清理后，读取必须报告失效；相同保存补齐后不增加修订。
TEST_F(PlatformAccessTest, DetectsMissingPolicyAndRepairsSameConfiguration) {
  bool healthy = false;
  int applies = 0;
  auto check = [&](const pa::OrdinaryNetworkConfig &) {
    return pa::OrdinaryNetworkResult{healthy, healthy ? "" : "策略规则缺失", healthy ? "192.168.2.219" : ""};
  };
  pa::PlatformAccessSupervisor control(path, {}, [&](const pa::OrdinaryNetworkConfig &) {
    ++applies;
    healthy = true;
    return check({}); }, check);
  auto request = Request();
  request.set_enabled(false);
  request.mutable_network_config()->set_interface_name("eth0.102");
  request.mutable_network_config()->set_gateway("192.168.2.1");
  ModuleManagerProto::PlatformAccessStatus status;
  ASSERT_TRUE(control.Set(request, &status).ok());
  healthy = false;
  EXPECT_FALSE(Read(control).network_applied());
  request.set_expected_revision(1);
  ASSERT_TRUE(control.Set(request, &status).ok());
  EXPECT_EQ(status.revision(), 1);
  EXPECT_TRUE(status.network_applied());
  EXPECT_EQ(applies, 2);
  ASSERT_TRUE(control.Set(request, &status).ok());
  EXPECT_EQ(applies, 2);
}

// 保存执行器繁忙时读取快速返回明确未核验状态，不能等待慢网络命令。
TEST_F(PlatformAccessTest, ReadDoesNotWaitForSlowNetworkApply) {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false, release = false;
  pa::PlatformAccessSupervisor control(path, {}, [&](const pa::OrdinaryNetworkConfig &) {
    std::unique_lock lock(mutex);
    entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release; });
    return pa::OrdinaryNetworkResult{true, "", "192.168.2.219"}; }, [](const pa::OrdinaryNetworkConfig &) { return pa::OrdinaryNetworkResult{true, "", ""}; });
  auto request = Request();
  request.mutable_network_config()->set_interface_name("eth0.102");
  request.mutable_network_config()->set_gateway("192.168.2.1");
  std::jthread writer([&] { ModuleManagerProto::PlatformAccessStatus status; EXPECT_TRUE(control.Set(request, &status).ok()); });
  {
    std::unique_lock lock(mutex);
    EXPECT_TRUE(condition.wait_for(lock, 2s, [&] { return entered; }));
  }
  const auto started = std::chrono::steady_clock::now();
  const auto status = Read(control);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
  EXPECT_FALSE(status.network_applied());
  EXPECT_FALSE(status.network_error().empty());
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
}

// 周期核验修复规则丢失且不重启正在工作的健康平台连接任务。
TEST_F(PlatformAccessTest, PeriodicRepairKeepsPlatformWorkerAndRevision) {
  std::atomic_bool healthy{false};
  std::atomic_int applies{0}, starts{0};
  auto check = [&](const pa::OrdinaryNetworkConfig &) {
    return pa::OrdinaryNetworkResult{healthy.load(), healthy ? "" : "策略规则缺失", "192.168.2.219"};
  };
  pa::PlatformAccessSupervisor control(path, [&](const pa::Config &, std::stop_token stop, pa::PlatformStatusObserver observer) {
    ++starts;
    observer(true, "");
    while (!stop.stop_requested()) std::this_thread::sleep_for(5ms); }, [&](const pa::OrdinaryNetworkConfig &) { ++applies; healthy = true; return check({}); }, check);
  auto request = Request();
  request.mutable_network_config()->set_interface_name("eth0.102");
  request.mutable_network_config()->set_gateway("192.168.2.1");
  ModuleManagerProto::PlatformAccessStatus status;
  ASSERT_TRUE(control.Set(request, &status).ok());
  std::jthread background([&](std::stop_token stop) { control.Run(stop); });
  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!starts && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  healthy = false;
  deadline = std::chrono::steady_clock::now() + 7s;
  while (applies < 2 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  EXPECT_EQ(applies.load(), 2);
  EXPECT_EQ(starts.load(), 1);
  EXPECT_EQ(Read(control).revision(), 1);
  EXPECT_TRUE(Read(control).online());
  background.request_stop();
}
// 后台恢复占用执行器时，保存限时返回忙碌，不写配置修订或执行第二次网络应用。
TEST_F(PlatformAccessTest, SaveDoesNotQueueBehindSlowBackgroundRepair) {
  pa::StoredPlatformAccess stored;
  stored.revision = 1;
  stored.config.networkConfig.interfaceName = "eth0.102";
  stored.config.networkConfig.gateway = "192.168.2.1";
  ASSERT_EQ(pa::PlatformAccessStore(path).Save(stored), pa::PlatformAccessSave::Durable);
  const auto original = [&] {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false, release = false;
  std::atomic_int applies{0};
  pa::PlatformAccessSupervisor control(path, {}, [&](const pa::OrdinaryNetworkConfig &) {
    ++applies;
    std::unique_lock lock(mutex);
    entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release; });
    return pa::OrdinaryNetworkResult{true, "", "192.168.2.219"}; }, [](const pa::OrdinaryNetworkConfig &) { return pa::OrdinaryNetworkResult{false, "规则缺失", ""}; });
  std::jthread background([&](std::stop_token stop) { control.Run(stop); });
  {
    std::unique_lock lock(mutex);
    EXPECT_TRUE(condition.wait_for(lock, 2s, [&] { return entered; }));
  }
  ModuleManagerProto::PlatformAccessStatus status;
  const auto started = std::chrono::steady_clock::now();
  const auto result = control.Set(Request(1), &status);
  EXPECT_EQ(result.error_code(), grpc::StatusCode::UNAVAILABLE);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
  EXPECT_EQ(Read(control).revision(), 1);
  EXPECT_EQ(applies.load(), 1);
  std::ifstream input(path);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()), original);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  background.request_stop();
  condition.notify_all();
}
}  // namespace
