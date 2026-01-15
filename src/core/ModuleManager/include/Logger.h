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
#include <string_view>
#include <tuple>
#include <utility>

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

namespace detail {
template <typename... Args>
std::string formatLog(std::string_view fmt, Args&&... args) {
  auto argsTuple = std::make_tuple(std::forward<Args>(args)...);
  return std::apply(
      [&fmt](auto&... values) {
        return std::vformat(fmt, std::make_format_args(values...));
      },
      argsTuple);
}
}  // namespace detail
}  // namespace ModuleManager

#define LOG_TRACE(fmt, ...)                                                        \
  BOOST_LOG_STREAM_SEV(::ModuleManager::Logger::get(), boost::log::trivial::trace) \
      << "[" << __FILE_NAME__ << ": " << __LINE__ << "] "                          \
      << "[" << __FUNCTION__ << "] "                                               \
      << ::ModuleManager::detail::formatLog(fmt __VA_OPT__(,) __VA_ARGS__)

#define LOG_DEBUG(fmt, ...)                                                          \
  BOOST_LOG_STREAM_SEV((::ModuleManager::Logger::get()), boost::log::trivial::debug) \
      << "[" << __FILE_NAME__ << ": " << __LINE__ << "] "                            \
      << "[" << __FUNCTION__ << "] "                                                 \
      << ::ModuleManager::detail::formatLog(fmt __VA_OPT__(,) __VA_ARGS__)

#define LOG_INFO(fmt, ...)                                                        \
  BOOST_LOG_STREAM_SEV(::ModuleManager::Logger::get(), boost::log::trivial::info) \
      << "[" << __FILE_NAME__ << ": " << __LINE__ << "] "                         \
      << "[" << __FUNCTION__ << "] "                                              \
      << ::ModuleManager::detail::formatLog(fmt __VA_OPT__(,) __VA_ARGS__)

#define LOG_WARNING(fmt, ...)                                                          \
  BOOST_LOG_STREAM_SEV((::ModuleManager::Logger::get()), boost::log::trivial::warning) \
      << "[" << __FILE_NAME__ << ": " << __LINE__ << "] "                              \
      << "[" << __FUNCTION__ << "] "                                                   \
      << ::ModuleManager::detail::formatLog(fmt __VA_OPT__(,) __VA_ARGS__)

#define LOG_ERROR(fmt, ...)                                                          \
  BOOST_LOG_STREAM_SEV((::ModuleManager::Logger::get()), boost::log::trivial::error) \
      << "[" << __FILE_NAME__ << ": " << __LINE__ << "] "                            \
      << "[" << __FUNCTION__ << "] "                                                 \
      << ::ModuleManager::detail::formatLog(fmt __VA_OPT__(,) __VA_ARGS__)

#define LOG_FATAL(fmt, ...)                                                          \
  BOOST_LOG_STREAM_SEV((::ModuleManager::Logger::get()), boost::log::trivial::fatal) \
      << "[" << __FILE_NAME__ << ": " << __LINE__ << "] "                            \
      << "[" << __FUNCTION__ << "] "                                                 \
      << ::ModuleManager::detail::formatLog(fmt __VA_OPT__(,) __VA_ARGS__)
