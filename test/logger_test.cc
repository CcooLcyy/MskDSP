#include <gtest/gtest.h>

#include <boost/log/core.hpp>

#include <array>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include "Logger.h"

namespace {
namespace fs = std::filesystem;

constexpr char kDefaultModuleName[] = "moduleManager";
constexpr char kOtherModuleName[] = "loggerTestModule";
constexpr char kInactiveModuleName[] = "inactiveLoggerModule";
constexpr char kQuotaModuleName[] = "quotaLoggerModule";
constexpr char kLogFileName[] = "moduleManager.log";
constexpr std::uintmax_t kArchiveLimitBytesForTest = 500ull * 1024ull * 1024ull;
constexpr std::uintmax_t kQuotaFixtureBytes = 200ull * 1024ull * 1024ull;

fs::path LogDir() { return fs::path("log"); }
fs::path ModuleLogDir(std::string_view moduleName) { return LogDir() / std::string(moduleName); }
fs::path ActiveLogPath(std::string_view moduleName = kDefaultModuleName) {
  return ModuleLogDir(moduleName) / (std::string(moduleName) + ".log");
}

std::chrono::sys_days LocalToday() {
  std::time_t now = std::time(nullptr);
  std::tm local_tm{};
  localtime_r(&now, &local_tm);
  std::chrono::year_month_day ymd{
      std::chrono::year{local_tm.tm_year + 1900},
      std::chrono::month{static_cast<unsigned>(local_tm.tm_mon + 1)},
      std::chrono::day{static_cast<unsigned>(local_tm.tm_mday)}};
  return std::chrono::sys_days{ymd};
}

std::string DateString(std::chrono::sys_days day) {
  std::chrono::year_month_day ymd{day};
  std::ostringstream oss;
  oss << std::setw(4) << std::setfill('0') << static_cast<int>(ymd.year()) << '-'
      << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ymd.month()) << '-'
      << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ymd.day());
  return oss.str();
}

fs::path RotatedLogPathForModule(std::string_view moduleName, const std::string &date, int index) {
  std::ostringstream oss;
  oss << moduleName << "_" << date << "_12-00-00_" << index << ".log";
  return ModuleLogDir(moduleName) / oss.str();
}

fs::path RotatedLogPathForDate(const std::string &date, int index) {
  return RotatedLogPathForModule(kDefaultModuleName, date, index);
}

void WriteTextFile(const fs::path &path, std::string_view content) {
  fs::create_directories(path.parent_path());
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  ofs << content;
}

std::string ReadTextFile(const fs::path &path) {
  std::ifstream ifs(path, std::ios::binary);
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

void WriteSparseFile(const fs::path &path, std::uintmax_t size) {
  fs::create_directories(path.parent_path());
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(ofs.is_open());
  ofs.close();
  std::error_code ec;
  fs::resize_file(path, size, ec);
  ASSERT_FALSE(ec) << ec.message();
}

class LoggerTest : public ::testing::Test {
protected:
  static inline std::string today_;
  static inline std::string old_date_;
  static inline fs::path legacy_log_;
  static inline fs::path legacy_log_gz_;
  static inline fs::path old_log_;
  static inline fs::path old_log_gz_;
  static inline fs::path inactive_legacy_log_;
  static inline fs::path inactive_legacy_log_gz_;
  static inline fs::path inactive_old_log_gz_;
  static inline std::array<fs::path, 3> quota_logs_;

  static void SetUpTestSuite() {
    fs::remove_all(LogDir());
    fs::create_directories(LogDir());

    WriteTextFile(ActiveLogPath(), "previous-log\n");

    today_ = DateString(LocalToday());
    legacy_log_ = RotatedLogPathForDate(today_, 1);
    WriteTextFile(legacy_log_, "legacy-log\n");
    legacy_log_gz_ = legacy_log_;
    legacy_log_gz_ += ".gz";

    old_date_ = DateString(LocalToday() - std::chrono::days(61));
    old_log_ = RotatedLogPathForDate(old_date_, 2);
    WriteTextFile(old_log_, "old-log\n");
    old_log_gz_ = old_log_;
    old_log_gz_ += ".gz";

    inactive_legacy_log_ = RotatedLogPathForModule(kInactiveModuleName, today_, 1);
    WriteTextFile(inactive_legacy_log_, "inactive-legacy-log\n");
    inactive_legacy_log_gz_ = inactive_legacy_log_;
    inactive_legacy_log_gz_ += ".gz";

    auto inactive_old_log = RotatedLogPathForModule(kInactiveModuleName, old_date_, 2);
    inactive_old_log_gz_ = inactive_old_log;
    inactive_old_log_gz_ += ".gz";
    WriteTextFile(inactive_old_log_gz_, "inactive-old-log\n");

    for (int index = 0; index < static_cast<int>(quota_logs_.size()); ++index) {
      quota_logs_[index] = RotatedLogPathForModule(kQuotaModuleName, today_, index);
      quota_logs_[index] += ".gz";
      WriteSparseFile(quota_logs_[index], kQuotaFixtureBytes);
    }

    ModuleManager::Logger::init(LogDir().string(), kLogFileName);
    LOG_INFO("logger_test_init");
    boost::log::core::get()->flush();
  }
};
}  // 命名空间结束

// 验证：初始化后日志仍保留已有内容，并继续追加写入新日志。
TEST_F(LoggerTest, KeepsExistingLogContentOnInit) {
  auto content = ReadTextFile(ActiveLogPath());
  EXPECT_NE(content.find("previous-log"), std::string::npos);
  EXPECT_NE(content.find("logger_test_init"), std::string::npos);
}

// 验证：初始化会压缩历史日志为 .gz 文件。
TEST_F(LoggerTest, CompressesLegacyLogsOnInit) {
  EXPECT_FALSE(fs::exists(legacy_log_));
  EXPECT_TRUE(fs::exists(legacy_log_gz_));
}

// 验证：初始化会清理超过 60 天的历史日志。
TEST_F(LoggerTest, RemovesLogsOlderThanRetention) {
  EXPECT_FALSE(fs::exists(old_log_));
  EXPECT_FALSE(fs::exists(old_log_gz_));
}

// 验证：初始化会维护未启动模块目录中的历史日志。
TEST_F(LoggerTest, MaintainsInactiveModuleDirectoryOnInit) {
  EXPECT_FALSE(fs::exists(inactive_legacy_log_));
  EXPECT_TRUE(fs::exists(inactive_legacy_log_gz_));
  EXPECT_FALSE(fs::exists(inactive_old_log_gz_));
}

// 验证：初始化会按目录归档容量限制删除最早的归档文件，并保留活动日志之外的较新归档。
TEST_F(LoggerTest, EnforcesArchiveLimitPerDirectoryOnInit) {
  EXPECT_FALSE(fs::exists(quota_logs_[0]));
  EXPECT_TRUE(fs::exists(quota_logs_[1]));
  EXPECT_TRUE(fs::exists(quota_logs_[2]));

  std::uintmax_t totalSize = 0;
  for (const auto &path : quota_logs_) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
      totalSize += fs::file_size(path, ec);
      ASSERT_FALSE(ec) << ec.message();
    }
  }
  EXPECT_LE(totalSize, kArchiveLimitBytesForTest);
}

// 验证：不同模块日志写入各自目录，默认模块日志不包含其他模块内容。
TEST_F(LoggerTest, WritesLogsToModuleSpecificFile) {
  auto other_log_path = ActiveLogPath(kOtherModuleName);
  std::error_code ec;
  fs::remove_all(other_log_path.parent_path(), ec);

  {
    ModuleManager::LogModuleScope scope(kOtherModuleName);
    LOG_INFO("logger_test_other_module");
  }
  boost::log::core::get()->flush();

  auto other_content = ReadTextFile(other_log_path);
  EXPECT_NE(other_content.find("logger_test_other_module"), std::string::npos);

  auto default_content = ReadTextFile(ActiveLogPath());
  EXPECT_EQ(default_content.find("logger_test_other_module"), std::string::npos);
}
