#include "Logger.h"

#include <boost/filesystem/path.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/log/attributes/function.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/make_shared.hpp>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <string>
#include <string_view>
#include <utility>

namespace ModuleManager {
namespace {
constexpr std::uintmax_t kRotationSizeBytes = 10 * 1024 * 1024;
constexpr int kRetentionDays = 60;
constexpr const char *kDefaultModuleName = "moduleManager";

namespace sink_file = boost::log::sinks::file;
namespace log_fs = boost::filesystem;

struct LogFileInfo {
  std::string base_name;
  std::string extension;
  std::string prefix;
  std::filesystem::path active_pattern;
  std::filesystem::path rotated_pattern;
  std::filesystem::path active_file;
};

boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level> &loggerInstance() {
  static boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level> logger;
  return logger;
}

thread_local std::string currentLogModuleName;
std::filesystem::path logBaseDir{"./log"};
std::string logExtension{".log"};
std::mutex sinkMutex;
std::unordered_set<std::string> moduleSinks;

std::string NormalizeModuleName(std::string_view moduleName) {
  if (moduleName.empty() || moduleName == "-") {
    return std::string(kDefaultModuleName);
  }
  return std::string(moduleName);
}

std::string NormalizeLogExtension(std::string_view fileName) {
  std::filesystem::path name_path(fileName);
  std::string extension = name_path.extension().string();
  if (extension.empty()) {
    return ".log";
  }
  return extension;
}

std::string moduleNameAttributeValue() {
  return NormalizeModuleName(currentLogModuleName);
}

boost::log::formatter BuildLogFormatter() {
  namespace logging = boost::log;
  namespace expr = boost::log::expressions;
  return expr::stream
      << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S")
      << " [" << logging::trivial::severity << "] "
      << " [" << expr::if_(expr::has_attr<std::string>(kLogTagModule))[expr::stream << expr::attr<std::string>(kLogTagModule)].else_[expr::stream << kDefaultModuleName] << "] "
      << expr::smessage;
}

std::chrono::sys_days CurrentLocalDay() {
  std::time_t now = std::time(nullptr);
  std::tm local_tm{};
  localtime_r(&now, &local_tm);
  std::chrono::year_month_day ymd{
      std::chrono::year{local_tm.tm_year + 1900},
      std::chrono::month{static_cast<unsigned>(local_tm.tm_mon + 1)},
      std::chrono::day{static_cast<unsigned>(local_tm.tm_mday)}};
  return std::chrono::sys_days{ymd};
}

std::optional<int> ParseNumber(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  int value = 0;
  for (char ch : text) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return std::nullopt;
    }
    value = value * 10 + (ch - '0');
  }
  return value;
}

std::optional<std::chrono::sys_days> ParseLogDate(std::string_view file_name, std::string_view prefix) {
  if (!file_name.starts_with(prefix) || file_name.size() < prefix.size() + 10) {
    return std::nullopt;
  }
  auto date = file_name.substr(prefix.size(), 10);
  if (date[4] != '-' || date[7] != '-') {
    return std::nullopt;
  }
  auto year = ParseNumber(date.substr(0, 4));
  auto month = ParseNumber(date.substr(5, 2));
  auto day = ParseNumber(date.substr(8, 2));
  if (!year || !month || !day) {
    return std::nullopt;
  }
  std::chrono::year_month_day ymd{
      std::chrono::year{*year},
      std::chrono::month{static_cast<unsigned>(*month)},
      std::chrono::day{static_cast<unsigned>(*day)}};
  if (!ymd.ok()) {
    return std::nullopt;
  }
  return std::chrono::sys_days{ymd};
}

LogFileInfo BuildLogFileInfo(const std::filesystem::path &log_dir, const std::string &file_name) {
  std::filesystem::path name_path = std::filesystem::path(file_name).filename();
  std::string base_name = name_path.stem().string();
  std::string extension = name_path.extension().string();
  if (base_name.empty()) {
    base_name = name_path.string();
  }
  std::string prefix = base_name + "_";
  std::filesystem::path active_pattern = log_dir / (base_name + extension);
  std::filesystem::path rotated_pattern = log_dir / (base_name + "_%Y-%m-%d_%H-%M-%S_%N" + extension);
  std::filesystem::path active_file = active_pattern;
  return {std::move(base_name), std::move(extension), std::move(prefix), std::move(active_pattern),
          std::move(rotated_pattern), std::move(active_file)};
}

bool CompressLogFile(const std::filesystem::path &path) {
  std::error_code ec;
  if (path.empty() || !std::filesystem::exists(path, ec)) {
    return false;
  }
  std::filesystem::path gz_path = path;
  gz_path += ".gz";
  if (std::filesystem::exists(gz_path, ec)) {
    std::filesystem::remove(path, ec);
    return true;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return false;
  }
  std::ofstream output(gz_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return false;
  }
  try {
    boost::iostreams::filtering_ostream out;
    out.push(boost::iostreams::gzip_compressor());
    out.push(output);
    boost::iostreams::copy(input, out);
  } catch (const std::exception &) {
    std::filesystem::remove(gz_path, ec);
    return false;
  }
  std::filesystem::remove(path, ec);
  return true;
}

void CleanupOldLogs(const std::filesystem::path &log_dir, std::string_view prefix) {
  std::error_code ec;
  auto cutoff = CurrentLocalDay() - std::chrono::days(kRetentionDays);
  for (const auto &entry : std::filesystem::directory_iterator(log_dir, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    auto file_name = entry.path().filename().string();
    auto date = ParseLogDate(file_name, prefix);
    if (!date) {
      continue;
    }
    if (*date < cutoff) {
      std::filesystem::remove(entry.path(), ec);
    }
  }
}

void CompressLegacyLogs(const std::filesystem::path &log_dir, std::string_view prefix,
                        const std::filesystem::path &active_path) {
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(log_dir, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto &path = entry.path();
    if (path == active_path || path.extension() == ".gz") {
      continue;
    }
    auto file_name = path.filename().string();
    if (!file_name.starts_with(prefix)) {
      continue;
    }
    CompressLogFile(path);
  }
}

class CompressedFileCollector final : public sink_file::collector {
public:
  CompressedFileCollector(std::filesystem::path log_dir, std::string prefix) :
    log_dir_(std::move(log_dir)),
    prefix_(std::move(prefix)) {
  }

  void store_file(log_fs::path const &src_path) override {
    std::scoped_lock lock(mutex_);
    std::filesystem::path path(src_path.string());
    auto file_name = path.filename().string();
    if (file_name.starts_with(prefix_) && path.extension() != ".gz") {
      CompressLogFile(path);
    }
    CleanupOldLogs(log_dir_, prefix_);
  }

  bool is_in_storage(log_fs::path const &src_path) const override {
    std::filesystem::path path(src_path.string());
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      return true;
    }
    std::filesystem::path gz_path = path;
    gz_path += ".gz";
    return std::filesystem::exists(gz_path, ec);
  }

  sink_file::scan_result scan_for_files(sink_file::scan_method method, log_fs::path const &) override {
    sink_file::scan_result result;
    if (method == sink_file::no_scan) {
      return result;
    }
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(log_dir_, ec)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      auto file_name = entry.path().filename().string();
      if (file_name.starts_with(prefix_)) {
        ++result.found_count;
      }
    }
    return result;
  }

private:
  std::filesystem::path log_dir_;
  std::string prefix_;
  mutable std::mutex mutex_;
};

void CreateModuleSinkLocked(const std::string &module_name) {
  namespace logging = boost::log;
  namespace expr = boost::log::expressions;
  namespace keywords = boost::log::keywords;

  std::filesystem::path module_dir = logBaseDir / module_name;
  if (!std::filesystem::exists(module_dir)) {
    std::filesystem::create_directories(module_dir);
  }
  std::string file_name = module_name + logExtension;
  auto log_info = BuildLogFileInfo(module_dir, file_name);
  CompressLegacyLogs(module_dir, log_info.prefix, log_info.active_file);
  CleanupOldLogs(module_dir, log_info.prefix);

  using backend_t = logging::sinks::text_file_backend;
  using sink_t = logging::sinks::synchronous_sink<backend_t>;
  auto backend = boost::make_shared<backend_t>(
      keywords::file_name = log_info.active_pattern.string(),
      keywords::target_file_name = log_info.rotated_pattern.string(),
      keywords::open_mode = std::ios_base::app,
      keywords::rotation_size = kRotationSizeBytes,
      keywords::time_based_rotation = sink_file::rotation_at_time_point(0, 0, 0),
      keywords::auto_flush = true);
  backend->set_file_collector(boost::make_shared<CompressedFileCollector>(module_dir, log_info.prefix));
  auto sink = boost::make_shared<sink_t>(backend);
  sink->set_formatter(BuildLogFormatter());
  sink->set_filter(expr::attr<std::string>(kLogTagModule) == module_name);
  logging::core::get()->add_sink(sink);
}
}  // namespace

std::once_flag Logger::initFlag_;

LogModuleScope::LogModuleScope(std::string moduleName) :
  prevModuleName_(std::exchange(currentLogModuleName, std::move(moduleName))) {
  Logger::ensureModuleSink(currentLogModuleName);
}

LogModuleScope::~LogModuleScope() {
  currentLogModuleName = std::move(prevModuleName_);
}

void Logger::ensureModuleSink(const std::string &moduleName) {
  init();
  auto normalized = NormalizeModuleName(moduleName);
  {
    std::scoped_lock lock(sinkMutex);
    if (moduleSinks.contains(normalized)) {
      return;
    }
    CreateModuleSinkLocked(normalized);
    moduleSinks.insert(normalized);
  }
  std::filesystem::path module_dir = logBaseDir / normalized;
  BOOST_LOG_STREAM_SEV(loggerInstance(), boost::log::trivial::info)
      << "模块日志输出已启用: module=" << normalized
      << ", dir=" << module_dir.string()
      << ", file=" << (module_dir / (normalized + logExtension)).string();
}

void Logger::init(const std::string &logDir, const std::string &fileName) {
  std::call_once(initFlag_, [&]() {
    namespace logging = boost::log;
    namespace keywords = boost::log::keywords;

    logBaseDir = logDir.empty() ? std::filesystem::path("./log") : std::filesystem::path(logDir);
    logExtension = NormalizeLogExtension(fileName);

    logging::add_common_attributes();
    logging::core::get()->add_global_attribute(kLogTagModule, logging::attributes::make_function(&moduleNameAttributeValue));

    logging::add_console_log(std::clog, keywords::format = BuildLogFormatter());
    logging::core::get()->set_filter(logging::trivial::severity >= logging::trivial::info);

    {
      std::scoped_lock lock(sinkMutex);
      if (!moduleSinks.contains(kDefaultModuleName)) {
        CreateModuleSinkLocked(kDefaultModuleName);
        moduleSinks.insert(std::string(kDefaultModuleName));
      }
    }
    std::filesystem::path module_dir = logBaseDir / kDefaultModuleName;
    BOOST_LOG_STREAM_SEV(loggerInstance(), boost::log::trivial::info)
        << "模块日志输出已启用: module=" << kDefaultModuleName
        << ", dir=" << module_dir.string()
        << ", file=" << (module_dir / (std::string(kDefaultModuleName) + logExtension)).string();

#ifndef NDEBUG
    BOOST_LOG_STREAM_SEV(loggerInstance(), boost::log::trivial::info)
        << "日志格式: Debug 模式包含文件/行号";
#else
    BOOST_LOG_STREAM_SEV(loggerInstance(), boost::log::trivial::info)
        << "日志格式: 非 Debug 模式省略文件/行号";
#endif
    BOOST_LOG_STREAM_SEV(loggerInstance(), boost::log::trivial::info)
        << "日志格式: 已禁用函数名输出";
    BOOST_LOG_STREAM_SEV(loggerInstance(), boost::log::trivial::info)
        << "Log collector enabled: gzip compression + retention cleanup";
    BOOST_LOG_STREAM_SEV(loggerInstance(), boost::log::trivial::info)
        << "日志轮转配置: 按日切分, 保留" << kRetentionDays << "天, 历史日志自动压缩";
  });
}

boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level> &Logger::get() {
  init();
  return loggerInstance();
}
}  // namespace ModuleManager
