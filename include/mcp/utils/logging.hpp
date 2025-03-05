#ifndef MCP_UTILS_LOGGING_HPP_
#define MCP_UTILS_LOGGING_HPP_

#include <functional>
#include <memory>
#include <sstream>
#include <string>

namespace mcp {
namespace logging {

/**
 * @brief Log levels
 */
enum class Level { Trace, Debug, Info, Warning, Error, Fatal };

/**
 * @brief Convert a log level to a string
 *
 * @param level The log level
 * @return std::string The string representation
 */
std::string levelToString(Level level);

/**
 * @brief Parse a log level from a string
 *
 * @param level_str The string representation
 * @return Level The log level
 * @throws std::invalid_argument if the string is not a valid log level
 */
Level levelFromString(const std::string &level_str);

/**
 * @brief Log handler function type
 *
 * @param level The log level
 * @param message The log message
 * @param file The source file
 * @param line The source line
 */
using LogHandler = std::function<void(Level level, const std::string &message,
                                      const std::string &file, int line)>;

/**
 * @brief Set the global log level
 *
 * @param level The log level
 */
void setLevel(Level level);

/**
 * @brief Get the global log level
 *
 * @return Level The log level
 */
Level getLevel();

/**
 * @brief Set the global log handler
 *
 * @param handler The log handler
 */
void setHandler(LogHandler handler);

/**
 * @brief Log a message
 *
 * @param level The log level
 * @param message The log message
 * @param file The source file
 * @param line The source line
 */
void log(Level level, const std::string &message, const std::string &file = "",
         int line = 0);

/**
 * @brief Check if a log level is enabled
 *
 * @param level The log level
 * @return true if the level is enabled, false otherwise
 */
bool isEnabled(Level level);

/**
 * @brief Default log handler that logs to stderr
 *
 * @param level The log level
 * @param message The log message
 * @param file The source file
 * @param line The source line
 */
void defaultHandler(Level level, const std::string &message,
                    const std::string &file, int line);

} // namespace logging
} // namespace mcp

// Convenience macros for logging
#define MCP_LOG_TRACE(msg)                                                     \
  do {                                                                         \
    if (mcp::logging::isEnabled(mcp::logging::Level::Trace)) {                 \
      mcp::logging::log(mcp::logging::Level::Trace, msg, __FILE__, __LINE__);  \
    }                                                                          \
  } while (0)

#define MCP_LOG_DEBUG(msg)                                                     \
  do {                                                                         \
    if (mcp::logging::isEnabled(mcp::logging::Level::Debug)) {                 \
      mcp::logging::log(mcp::logging::Level::Debug, msg, __FILE__, __LINE__);  \
    }                                                                          \
  } while (0)

#define MCP_LOG_INFO(msg)                                                      \
  do {                                                                         \
    if (mcp::logging::isEnabled(mcp::logging::Level::Info)) {                  \
      mcp::logging::log(mcp::logging::Level::Info, msg, __FILE__, __LINE__);   \
    }                                                                          \
  } while (0)

#define MCP_LOG_WARNING(msg)                                                   \
  do {                                                                         \
    if (mcp::logging::isEnabled(mcp::logging::Level::Warning)) {               \
      mcp::logging::log(mcp::logging::Level::Warning, msg, __FILE__,           \
                        __LINE__);                                             \
    }                                                                          \
  } while (0)

#define MCP_LOG_ERROR(msg)                                                     \
  do {                                                                         \
    if (mcp::logging::isEnabled(mcp::logging::Level::Error)) {                 \
      mcp::logging::log(mcp::logging::Level::Error, msg, __FILE__, __LINE__);  \
    }                                                                          \
  } while (0)

#define MCP_LOG_FATAL(msg)                                                     \
  do {                                                                         \
    if (mcp::logging::isEnabled(mcp::logging::Level::Fatal)) {                 \
      mcp::logging::log(mcp::logging::Level::Fatal, msg, __FILE__, __LINE__);  \
    }                                                                          \
  } while (0)

#endif // MCP_UTILS_LOGGING_HPP_