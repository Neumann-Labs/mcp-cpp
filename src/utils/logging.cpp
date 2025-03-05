#include "mcp/utils/logging.hpp"
#include <atomic>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace mcp {
namespace logging {

namespace {
// Global log level
std::atomic<Level> g_level{Level::Info};

// Global log handler
std::mutex g_handler_mutex;
LogHandler g_handler = defaultHandler;
} // namespace

std::string levelToString(Level level) {
  switch (level) {
  case Level::Trace:
    return "TRACE";
  case Level::Debug:
    return "DEBUG";
  case Level::Info:
    return "INFO";
  case Level::Warning:
    return "WARNING";
  case Level::Error:
    return "ERROR";
  case Level::Fatal:
    return "FATAL";
  default:
    return "UNKNOWN";
  }
}

Level levelFromString(const std::string &level_str) {
  if (level_str == "TRACE" || level_str == "trace") {
    return Level::Trace;
  } else if (level_str == "DEBUG" || level_str == "debug") {
    return Level::Debug;
  } else if (level_str == "INFO" || level_str == "info") {
    return Level::Info;
  } else if (level_str == "WARNING" || level_str == "warning") {
    return Level::Warning;
  } else if (level_str == "ERROR" || level_str == "error") {
    return Level::Error;
  } else if (level_str == "FATAL" || level_str == "fatal") {
    return Level::Fatal;
  } else {
    throw std::invalid_argument("Invalid log level: " + level_str);
  }
}

void setLevel(Level level) { g_level = level; }

Level getLevel() { return g_level; }

void setHandler(LogHandler handler) {
  std::lock_guard<std::mutex> lock(g_handler_mutex);
  g_handler = handler ? handler : defaultHandler;
}

void log(Level level, const std::string &message, const std::string &file,
         int line) {
  if (level >= g_level) {
    LogHandler handler;
    {
      std::lock_guard<std::mutex> lock(g_handler_mutex);
      handler = g_handler;
    }
    handler(level, message, file, line);
  }
}

bool isEnabled(Level level) { return level >= g_level; }

void defaultHandler(Level level, const std::string &message,
                    const std::string &file, int line) {
  // Get current time
  std::time_t t = std::time(nullptr);
  std::tm tm = *std::localtime(&t);

  std::stringstream ss;
  ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");

  // Format: [LEVEL] [time] [file:line] message
  std::cerr << "[" << levelToString(level) << "] [" << ss.str() << "] ";

  if (!file.empty()) {
    std::cerr << "[" << file << ":" << line << "] ";
  }

  std::cerr << message << std::endl;
}

} // namespace logging
} // namespace mcp