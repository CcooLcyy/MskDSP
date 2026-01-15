#include "Logger.h"

#include <boost/log/attributes/function.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <filesystem>
#include <iostream>
#include <utility>

namespace ModuleManager {
namespace {
boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level> &loggerInstance() {
  static boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level> logger;
  return logger;
}

thread_local std::string currentLogModuleName;

std::string moduleNameAttributeValue() {
  if (currentLogModuleName.empty()) {
    return std::string("-");
  }
  return currentLogModuleName;
}
}  // namespace

std::once_flag Logger::initFlag_;

LogModuleScope::LogModuleScope(std::string moduleName) :
  prevModuleName_(std::exchange(currentLogModuleName, std::move(moduleName))) {
}

LogModuleScope::~LogModuleScope() {
  currentLogModuleName = std::move(prevModuleName_);
}

void Logger::init(const std::string &logDir, const std::string &fileName) {
  std::call_once(initFlag_, [&]() {
    namespace logging = boost::log;
    namespace expr = boost::log::expressions;
    namespace keywords = boost::log::keywords;

    std::filesystem::path dir(logDir);
    if (!std::filesystem::exists(dir)) {
      std::filesystem::create_directories(dir);
    }

    logging::add_common_attributes();
    logging::core::get()->add_global_attribute(kLogTagModule, logging::attributes::make_function(&moduleNameAttributeValue));
    auto formatter = expr::stream
        << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S")
        << " [" << logging::trivial::severity << "] "
        << " [" << expr::if_(expr::has_attr<std::string>(kLogTagModule))[expr::stream << expr::attr<std::string>(kLogTagModule)].else_[expr::stream << "-"] << "] "
        << expr::smessage;

    logging::add_file_log(
        keywords::file_name = (dir / fileName).string(),
        keywords::rotation_size = 10 * 1024 * 1024,
        keywords::auto_flush = true,
        keywords::format = formatter);
    logging::add_console_log(std::clog, keywords::format = formatter);
    logging::core::get()->set_filter(logging::trivial::severity >= logging::trivial::info);
  });
}

boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level> &Logger::get() {
  init();
  return loggerInstance();
}
}  // namespace ModuleManager
