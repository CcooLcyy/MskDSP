#pragma once

#include <format>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/manipulators/add_value.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <mutex>
#include <string>

namespace ModuleManager {
inline constexpr char kLogTagModule[] = "Module";

class LogModuleScope {
public:
  explicit LogModuleScope(std::string moduleName);
  ~LogModuleScope();

  LogModuleScope(const LogModuleScope &) = delete;
  LogModuleScope &operator=(const LogModuleScope &) = delete;
  LogModuleScope(LogModuleScope &&) = delete;
  LogModuleScope &operator=(LogModuleScope &&) = delete;

private:
  std::string prevModuleName_;
};

class Logger {
public:
  static void init(const std::string &logDir = "./log", const std::string &fileName = "module_manager.log");
  static boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level> &get();

private:
  static std::once_flag initFlag_;
};
}  // namespace ModuleManager

#define LOG_TRACE(fmt, ...)                                                        \
  BOOST_LOG_STREAM_SEV(::ModuleManager::Logger::get(), boost::log::trivial::trace) \
      << "[" << __FILE_NAME__ << ": " << __LINE__ << "] "                          \
      << "[" << __FUNCTION__ << "] "                                               \
      << std::vformat(fmt, std::make_format_args(__VA_ARGS__))

#define LOG_DEBUG(fmt, ...)                                                          \
  BOOST_LOG_STREAM_SEV((::ModuleManager::Logger::get()), boost::log::trivial::debug) \
      << "[" << __FILE_NAME__ << ": " << __LINE__ << "] "                            \
      << "[" << __FUNCTION__ << "] "                                                 \
      << std::vformat(fmt, std::make_format_args(__VA_ARGS__))

#define LOG_INFO(fmt, ...)                                                        \
  BOOST_LOG_STREAM_SEV(::ModuleManager::Logger::get(), boost::log::trivial::info) \
      << "[" << __FILE_NAME__ << ": " << __LINE__ << "] "                         \
      << "[" << __FUNCTION__ << "] "                                              \
      << std::vformat(fmt, std::make_format_args(__VA_ARGS__))

#define LOG_WARNING(fmt, ...)                                                          \
  BOOST_LOG_STREAM_SEV((::ModuleManager::Logger::get()), boost::log::trivial::warning) \
      << "[" << __FILE_NAME__ << ": " << __LINE__ << "] "                              \
      << "[" << __FUNCTION__ << "] "                                                   \
      << std::vformat(fmt, std::make_format_args(__VA_ARGS__))

#define LOG_ERROR(fmt, ...)                                                          \
  BOOST_LOG_STREAM_SEV((::ModuleManager::Logger::get()), boost::log::trivial::error) \
      << "[" << __FILE_NAME__ << ": " << __LINE__ << "] "                            \
      << "[" << __FUNCTION__ << "] "                                                 \
      << std::vformat(fmt, std::make_format_args(__VA_ARGS__))

#define LOG_FATAL(fmt, ...)                                                          \
  BOOST_LOG_STREAM_SEV((::ModuleManager::Logger::get()), boost::log::trivial::fatal) \
      << "[" << __FILE_NAME__ << ": " << __LINE__ << "] "                            \
      << "[" << __FUNCTION__ << "] "                                                 \
      << std::vformat(fmt, std::make_format_args(__VA_ARGS__))
