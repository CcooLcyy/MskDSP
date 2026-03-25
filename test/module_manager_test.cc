#include <grpcpp/server_context.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <stop_token>
#include <string>

#include "ModuleManager.h"
#include "ModuleManagerGrpcService.h"

namespace {
namespace fs = std::filesystem;

constexpr const char *kDummyModuleName = "Dummy";

fs::path LibDir() {
  return fs::path("module");
}
fs::path ConfDir() {
  return fs::path("conf");
}
fs::path SocketDir() {
  return fs::path("socket");
}
fs::path LogDir() {
  return fs::path("log");
}
fs::path AutoStartConfigPath() {
  return ConfDir() / "module_manager.jsonc";
}

std::string DummyLibPrefix() {
  return std::string("lib") + kDummyModuleName + ".so";
}

std::string FindDummyLibFileName() {
  const auto prefix = DummyLibPrefix();
  for (const auto &entry : fs::directory_iterator(LibDir())) {
    if (entry.is_symlink() || !entry.is_regular_file()) {
      continue;
    }
    const auto name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0) {
      return name;
    }
  }
  return {};
}

void CleanTestEnvKeepDummyLib() {
  // 清理并重建 conf/socket/log，覆盖目录创建相关代码路径。
  fs::remove_all(ConfDir());
  fs::remove_all(SocketDir());
  fs::remove_all(LogDir());
  fs::create_directories(ConfDir());

  // 保留假模块共享库及其符号链接链路，移除测试过程中产生的其他产物。
  fs::create_directories(LibDir());
  const auto prefix = DummyLibPrefix();
  for (const auto &entry : fs::directory_iterator(LibDir())) {
    const auto name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0) {
      continue;
    }
    fs::remove_all(entry.path());
  }
}

void WriteAutoStartConfig(const std::string &content) {
  fs::create_directories(ConfDir());
  std::ofstream ofs(AutoStartConfigPath(), std::ios::trunc);
  ofs << content;
}

ModuleManagerProto::ModuleInfo FindModuleInfoByName(const ModuleManagerProto::ModuleInfos &infos, const std::string &name) {
  for (const auto &info : infos.module_info()) {
    if (info.module_name() == name) {
      return info;
    }
  }
  ADD_FAILURE() << "ModuleInfo not found for module_name=" << name;
  return {};
}

bool HasModuleInfoByName(const ModuleManagerProto::ModuleInfos &infos, const std::string &name) {
  for (const auto &info : infos.module_info()) {
    if (info.module_name() == name) {
      return true;
    }
  }
  return false;
}

bool HasUsableModuleInfo(const ModuleManagerProto::ModuleInfos &infos, const std::string &name) {
  for (const auto &info : infos.module_info()) {
    if (info.module_name() != name) {
      continue;
    }
    if (!info.manifest_error().empty()) {
      return false;
    }
    for (const auto &dependency : info.dependencies()) {
      if (!HasModuleInfoByName(infos, dependency.module_name())) {
        return false;
      }
    }
    return true;
  }
  return false;
}

struct TraceableModule {
  std::string moduleName;
  fs::path tracePath;
};

std::optional<TraceableModule> FindAvailableTraceableModule(const ModuleManagerProto::ModuleInfos &infos) {
  const std::vector<TraceableModule> candidates = {
      {"DataCenter", ConfDir() / "dataCenter" / "connections.pb"},
      {"IEC104", ConfDir() / "IEC104" / "links.pb"},
      {"ModbusRTU", ConfDir() / "ModbusRTU" / "links.pb"},
      {"DLT645", ConfDir() / "DLT645" / "links.pb"},
      {"AGC", ConfDir() / "AGC" / "groups.pb"}};
  for (const auto &candidate : candidates) {
    if (HasUsableModuleInfo(infos, candidate.moduleName)) {
      return candidate;
    }
  }
  return std::nullopt;
}

void TouchFile(const fs::path &path) {
  fs::create_directories(path.parent_path());
  std::ofstream ofs(path, std::ios::trunc);
  ofs << "trace";
}

fs::path BackupPath(const fs::path &path) {
  return fs::path(path.string() + ".bak");
}

fs::path TmpPath(const fs::path &path) {
  return fs::path(path.string() + ".tmp");
}

int CountRunningModuleByName(const ModuleManagerProto::ModuleRunningInfos &infos, const std::string &name) {
  int count = 0;
  for (const auto &info : infos.module_running_info()) {
    if (info.module_name() == name) {
      ++count;
    }
  }
  return count;
}

class ModuleManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    CleanTestEnvKeepDummyLib();
    ASSERT_TRUE(fs::exists(LibDir())) << "测试工作目录下必须存在 `module/`";

    // 确认假模块已构建并放置到 `./module`。
    ASSERT_FALSE(FindDummyLibFileName().empty())
        << "未在 `./module` 找到假模块共享库（期望前缀: " << DummyLibPrefix() << ")";
  }
};
}  // namespace

// 验证：getModuleInfos 会扫描 ./module 中的模块，并读取 manifest 与 manifest_error。
TEST_F(ModuleManagerTest, GetModuleInfosScansLibDirAndParsesVersion) {
  // 额外创建若干条目，覆盖扫描过滤与分支逻辑。
  std::ofstream(LibDir() / "libNoVersion.so").put('\n');
  std::ofstream(LibDir() / "ab.so.0.0.1").put('\n');   // ".so" 出现过早，应被忽略。
  fs::create_directory(LibDir() / "libDir.so.0.0.1");  // 不是普通文件，应被忽略。

  // 符号链接应被忽略。
  const auto dummyFile = FindDummyLibFileName();
  ASSERT_FALSE(dummyFile.empty());
  fs::create_symlink(dummyFile, LibDir() / "libSymlink.so.0.0.1");

  ModuleManager::ModuleManager mgr;
  const auto &infos = mgr.getModuleInfos();

  const auto dummyInfo = FindModuleInfoByName(infos, kDummyModuleName);
  EXPECT_EQ(dummyInfo.lib_name(), dummyFile);
  EXPECT_EQ(dummyInfo.version().version(), "0.0.1");
  EXPECT_EQ(dummyInfo.version().major(), "0");
  EXPECT_EQ(dummyInfo.version().minor(), "0");
  EXPECT_EQ(dummyInfo.version().patch(), "1");
  EXPECT_TRUE(dummyInfo.manifest_error().empty());

  const auto noVerInfo = FindModuleInfoByName(infos, "NoVersion");
  EXPECT_EQ(noVerInfo.lib_name(), "libNoVersion.so");
  EXPECT_TRUE(noVerInfo.version().version().empty());
  EXPECT_TRUE(noVerInfo.version().major().empty());
  EXPECT_TRUE(noVerInfo.version().minor().empty());
  EXPECT_TRUE(noVerInfo.version().patch().empty());
  EXPECT_FALSE(noVerInfo.manifest_error().empty());

  // 确认被过滤的条目不会出现在结果中。
  EXPECT_FALSE(HasModuleInfoByName(infos, "Symlink"));
  EXPECT_FALSE(HasModuleInfoByName(infos, "Dir"));
}

// 验证：loadModule/unloadModule 会启动/停止模块线程，并维护运行中模块列表。
TEST_F(ModuleManagerTest, LoadAndUnloadModuleUpdatesRunningInfos) {
  ModuleManager::ModuleManager mgr;
  const auto &infos = mgr.getModuleInfos();

  auto missing = ModuleManagerProto::ModuleInfo{};
  missing.set_module_name("Missing");
  missing.set_lib_name("libMissing.so.0.0.1");
  auto missingResult = mgr.loadModule(missing);
  EXPECT_FALSE(missingResult.ok());
  EXPECT_EQ(mgr.getModuleRunningInfos().module_running_info_size(), 0);

  const auto dummyInfo = FindModuleInfoByName(infos, kDummyModuleName);
  auto startResult = mgr.loadModule(dummyInfo);
  EXPECT_TRUE(startResult.ok());
  auto running = mgr.getModuleRunningInfos();
  ASSERT_EQ(running.module_running_info_size(), 1);
  EXPECT_EQ(running.module_running_info(0).module_name(), kDummyModuleName);
  EXPECT_EQ(running.module_running_info(0).lib_name(), dummyInfo.lib_name());
  EXPECT_FALSE(running.module_running_info(0).inner_grpc_server().empty());
  EXPECT_FALSE(running.module_running_info(0).outer_grpc_server().empty());

  auto wrongLibName = dummyInfo;
  wrongLibName.set_lib_name("libDummy.so.9.9.9");
  auto wrongStop = mgr.unloadModule(wrongLibName);
  EXPECT_FALSE(wrongStop.ok());
  EXPECT_EQ(mgr.getModuleRunningInfos().module_running_info_size(), 1);

  auto stopResult = mgr.unloadModule(dummyInfo);
  EXPECT_TRUE(stopResult.ok());
  EXPECT_EQ(mgr.getModuleRunningInfos().module_running_info_size(), 0);

  // 卸载未运行的模块应是空操作。
  auto stopAgain = mgr.unloadModule(dummyInfo);
  EXPECT_TRUE(stopAgain.ok());
}

// 验证：启动时读取 module_manager.jsonc 的 auto_start_modules 并自动加载模块。
TEST_F(ModuleManagerTest, AutoStartModulesFromJsonConfig) {
  WriteAutoStartConfig(R"jsonc(
{
  // 自动加载的模块列表
  "auto_start_modules": ["Dummy"]
}
)jsonc");

  ModuleManager::ModuleManager mgr;
  std::stop_source stopSource;
  stopSource.request_stop();
  mgr.start(stopSource.get_token());

  auto running = mgr.getModuleRunningInfos();
  ASSERT_EQ(running.module_running_info_size(), 1);
  EXPECT_EQ(running.module_running_info(0).module_name(), kDummyModuleName);

  const auto dummyInfo = FindModuleInfoByName(mgr.getModuleInfos(), kDummyModuleName);
  mgr.unloadModule(dummyInfo);
  EXPECT_EQ(mgr.getModuleRunningInfos().module_running_info_size(), 0);
}

// 验证：`CONFIG_PUSHER` 模式下即使存在持久化配置文件痕迹，也只按 jsonc 中的 auto_start_modules 启动模块。
TEST_F(ModuleManagerTest, ConfigPusherModeIgnoresPersistentTraceAutoStart) {
  ModuleManager::ModuleManager mgr;
  const auto &infos = mgr.getModuleInfos();
  const auto candidate = FindAvailableTraceableModule(infos);
  if (!candidate.has_value()) {
    GTEST_SKIP() << "当前测试环境未提供可验证持久化痕迹自动启动的真实模块";
  }

  TouchFile(candidate->tracePath);
  WriteAutoStartConfig(R"jsonc(
{
  "boot_config_mode": "CONFIG_PUSHER",
  "auto_start_modules": ["Dummy"]
}
)jsonc");

  std::stop_source stopSource;
  stopSource.request_stop();
  mgr.start(stopSource.get_token());

  const auto running = mgr.getModuleRunningInfos();
  EXPECT_EQ(CountRunningModuleByName(running, kDummyModuleName), 1);
  EXPECT_EQ(CountRunningModuleByName(running, candidate->moduleName), 0);

  const auto dummyInfo = FindModuleInfoByName(mgr.getModuleInfos(), kDummyModuleName);
  mgr.unloadModule(dummyInfo);
  EXPECT_EQ(mgr.getModuleRunningInfos().module_running_info_size(), 0);
}

// 验证：`CONFIG_PUSHER` 模式会在启动任何模块前删除受管 `.pb/.bak/.tmp` 持久化文件，但不会误删无关文件。
TEST_F(ModuleManagerTest, ConfigPusherModeCleansManagedPersistentFilesBeforeModuleStart) {
  const auto managed = ConfDir() / "DLT645" / "links.pb";
  const auto managedBak = BackupPath(managed);
  const auto managedTmp = TmpPath(managed);
  const auto unrelated = ConfDir() / "custom" / "keep.pb";
  TouchFile(managed);
  TouchFile(managedBak);
  TouchFile(managedTmp);
  TouchFile(unrelated);

  WriteAutoStartConfig(R"jsonc(
{
  "boot_config_mode": "CONFIG_PUSHER",
  "auto_start_modules": ["Dummy"]
}
)jsonc");

  ModuleManager::ModuleManager mgr;
  std::stop_source stopSource;
  stopSource.request_stop();
  mgr.start(stopSource.get_token());

  EXPECT_FALSE(fs::exists(managed));
  EXPECT_FALSE(fs::exists(managedBak));
  EXPECT_FALSE(fs::exists(managedTmp));
  EXPECT_TRUE(fs::exists(unrelated));

  const auto running = mgr.getModuleRunningInfos();
  EXPECT_EQ(CountRunningModuleByName(running, kDummyModuleName), 1);

  const auto dummyInfo = FindModuleInfoByName(mgr.getModuleInfos(), kDummyModuleName);
  mgr.unloadModule(dummyInfo);
}

// 验证：`UPPER` 模式不会执行启动前中央清场，受管 `.tmp` 痕迹文件会被保留。
TEST_F(ModuleManagerTest, UpperModeDoesNotCleanManagedTmpTraceFiles) {
  const auto managedTmp = TmpPath(ConfDir() / "DLT645" / "links.pb");
  TouchFile(managedTmp);

  WriteAutoStartConfig(R"jsonc(
{
  "boot_config_mode": "UPPER",
  "auto_start_modules": ["Dummy"]
}
)jsonc");

  ModuleManager::ModuleManager mgr;
  std::stop_source stopSource;
  stopSource.request_stop();
  mgr.start(stopSource.get_token());

  EXPECT_TRUE(fs::exists(managedTmp));
  const auto running = mgr.getModuleRunningInfos();
  EXPECT_EQ(CountRunningModuleByName(running, kDummyModuleName), 1);

  const auto dummyInfo = FindModuleInfoByName(mgr.getModuleInfos(), kDummyModuleName);
  mgr.unloadModule(dummyInfo);
}

// 验证：`UPPER` 模式下发现持久化配置文件痕迹时，会自动启动对应模块。
TEST_F(ModuleManagerTest, UpperModeAutoStartsModuleFromPersistentTrace) {
  ModuleManager::ModuleManager mgr;
  const auto &infos = mgr.getModuleInfos();
  const auto candidate = FindAvailableTraceableModule(infos);
  if (!candidate.has_value()) {
    GTEST_SKIP() << "当前测试环境未提供可验证持久化痕迹自动启动的真实模块";
  }

  TouchFile(candidate->tracePath);
  WriteAutoStartConfig(R"jsonc(
{
  "boot_config_mode": "UPPER"
}
)jsonc");

  std::stop_source stopSource;
  stopSource.request_stop();
  mgr.start(stopSource.get_token());

  const auto running = mgr.getModuleRunningInfos();
  EXPECT_EQ(CountRunningModuleByName(running, candidate->moduleName), 1);
}

// 验证：`auto_start_modules` 与持久化文件痕迹同时命中时，重复启动会被安全跳过。
TEST_F(ModuleManagerTest, UpperModeTraceAutoStartCoexistsWithAutoStartModules) {
  ModuleManager::ModuleManager mgr;
  const auto &infos = mgr.getModuleInfos();
  const auto candidate = FindAvailableTraceableModule(infos);
  if (!candidate.has_value()) {
    GTEST_SKIP() << "当前测试环境未提供可验证持久化痕迹自动启动的真实模块";
  }

  TouchFile(candidate->tracePath);
  WriteAutoStartConfig(std::string(R"jsonc(
{
  "boot_config_mode": "UPPER",
  "auto_start_modules": [")jsonc") +
                       candidate->moduleName + R"jsonc("]
}
)jsonc");

  std::stop_source stopSource;
  stopSource.request_stop();
  mgr.start(stopSource.get_token());

  const auto running = mgr.getModuleRunningInfos();
  EXPECT_EQ(CountRunningModuleByName(running, candidate->moduleName), 1);
}

// 验证：自动启动配置读取或解析失败时，会按安全模式回退为 `UPPER` 并继续按持久化文件痕迹自动启动模块。
TEST_F(ModuleManagerTest, InvalidAutoStartConfigFallsBackToUpperTraceAutoStart) {
  ModuleManager::ModuleManager mgr;
  const auto &infos = mgr.getModuleInfos();
  const auto candidate = FindAvailableTraceableModule(infos);
  if (!candidate.has_value()) {
    GTEST_SKIP() << "当前测试环境未提供可验证持久化痕迹自动启动的真实模块";
  }

  TouchFile(candidate->tracePath);
  WriteAutoStartConfig(R"jsonc(
{
  "boot_config_mode":
)jsonc");

  std::stop_source stopSource;
  stopSource.request_stop();
  mgr.start(stopSource.get_token());

  const auto running = mgr.getModuleRunningInfos();
  EXPECT_EQ(CountRunningModuleByName(running, candidate->moduleName), 1);
}

// 验证：saveModuleStartConfig 会写入 ./conf/modConf.bin，且可反序列化回原数据。
TEST_F(ModuleManagerTest, SaveModuleStartConfigWritesConfigBin) {
  ModuleManager::ModuleManager mgr;
  const auto &infos = mgr.getModuleInfos();
  const auto dummyInfo = FindModuleInfoByName(infos, kDummyModuleName);

  ModuleManagerProto::ModuleInfos config;
  config.add_module_info()->CopyFrom(dummyInfo);

  mgr.saveModuleStartConfig(config);

  const auto bin = ConfDir() / "modConf.bin";
  ASSERT_TRUE(fs::exists(bin));

  std::ifstream ifs(bin, std::ios::binary);
  ASSERT_TRUE(ifs.is_open());

  ModuleManagerProto::ModuleInfos loaded;
  ASSERT_TRUE(loaded.ParseFromIstream(&ifs));
  EXPECT_EQ(loaded.SerializeAsString(), config.SerializeAsString());
}

// 验证：ModuleManagerGrpcService 将 RPC 调用正确委派给 ModuleManager。
TEST_F(ModuleManagerTest, GrpcServiceDelegatesToModuleManager) {
  ModuleManager::ModuleManager mgr;
  ModuleManager::ModuleManagerServiceImpl service;
  service.getModuleManager(&mgr);

  grpc::ServerContext context;
  ModuleManagerProto::Empty empty;

  ModuleManagerProto::ModuleInfos infos;
  ASSERT_TRUE(service.GetModuleInfo(&context, &empty, &infos).ok());
  const auto dummyInfo = FindModuleInfoByName(infos, kDummyModuleName);

  ModuleManagerProto::Empty out;
  ASSERT_TRUE(service.StartModule(&context, &dummyInfo, &out).ok());
  EXPECT_EQ(mgr.getModuleRunningInfos().module_running_info_size(), 1);

  ModuleManagerProto::ModuleRunningInfos running;
  ASSERT_TRUE(service.GetRunningModuleInfo(&context, &empty, &running).ok());
  EXPECT_EQ(running.module_running_info_size(), 1);

  ASSERT_TRUE(service.SaveModuleStartConfig(&context, &infos, &out).ok());
  EXPECT_TRUE(fs::exists(ConfDir() / "modConf.bin"));

  ASSERT_TRUE(service.StopModule(&context, &dummyInfo, &out).ok());
  EXPECT_EQ(mgr.getModuleRunningInfos().module_running_info_size(), 0);

  ASSERT_TRUE(service.UploadModule(&context, &empty, &out).ok());
  ASSERT_TRUE(service.DeleteModule(&context, &dummyInfo, &out).ok());
}
