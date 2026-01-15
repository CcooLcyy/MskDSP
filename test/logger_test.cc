#include <gtest/gtest.h>

#include <boost/log/core.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include "Logger.h"

namespace {
namespace fs = std::filesystem;

constexpr char kLogFileName[] = "module_manager.log";
constexpr char kLogPrefix[] = "module_manager_";

fs::path LogDir() { return fs::path("log"); }
fs::path ActiveLogPath() { return LogDir() / kLogFileName; }

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

fs::path RotatedLogPathForDate(const std::string &date, int index) {
  std::ostringstream oss;
  oss << kLogPrefix << date << "_12-00-00_" << index << ".log";
  return LogDir() / oss.str();
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

class LoggerTest : public ::testing::Test {
protected:
  static inline std::string today_;
  static inline std::string old_date_;
  static inline fs::path legacy_log_;
  static inline fs::path legacy_log_gz_;
  static inline fs::path old_log_;
  static inline fs::path old_log_gz_;

  static void SetUpTestSuite() {
    fs::remove_all(LogDir());
    fs::create_directories(LogDir());

    WriteTextFile(ActiveLogPath(), "previous-log\n");

    today_ = DateString(LocalToday());
    legacy_log_ = RotatedLogPathForDate(today_, 1);
    WriteTextFile(legacy_log_, "legacy-log\n");
    legacy_log_gz_ = legacy_log_;
    legacy_log_gz_ += ".gz";

    old_date_ = DateString(LocalToday() - std::chrono::days(31));
    old_log_ = RotatedLogPathForDate(old_date_, 2);
    WriteTextFile(old_log_, "old-log\n");
    old_log_gz_ = old_log_;
    old_log_gz_ += ".gz";

    ModuleManager::Logger::init(LogDir().string(), kLogFileName);
    LOG_INFO("logger_test_init");
    boost::log::core::get()->flush();
  }
};
}  // namespace

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

// 验证：初始化会清理超过 30 天的历史日志。
TEST_F(LoggerTest, RemovesLogsOlderThanRetention) {
  EXPECT_FALSE(fs::exists(old_log_));
  EXPECT_FALSE(fs::exists(old_log_gz_));
}
