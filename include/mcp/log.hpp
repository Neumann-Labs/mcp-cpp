// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal logging for the mcp-cpp library itself. This is *not* the MCP
// protocol-level logging that servers use to emit log messages to clients
// (see `notifications/message` in the spec). This module is for diagnostic
// output from the SDK to stderr — never to stdout, because stdout is owned by
// the stdio transport.

#include <atomic>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>

namespace mcp {

enum class LogLevel : int {
    trace = 0,
    debug = 1,
    info  = 2,
    warn  = 3,
    error = 4,
    off   = 5,
};

[[nodiscard]] std::string_view to_string(LogLevel l) noexcept;

/// Sink signature: receives a fully-formatted line (no trailing newline).
/// The sink is invoked synchronously from whichever thread emitted the log.
/// The default sink writes a timestamped line to `stderr`.
using LogSink = std::function<void(LogLevel, std::string_view)>;

/// Replace the global log sink. Pass `nullptr` to restore the default.
/// Thread-safe.
void set_log_sink(LogSink sink);

/// Set the global minimum level. Calls below this level are dropped before
/// any message is formatted. Thread-safe (atomic).
void set_log_level(LogLevel level) noexcept;

[[nodiscard]] LogLevel log_level() noexcept;

/// Low-level entry point. Prefer the `MCP_LOG_*` macros below, which
/// short-circuit at the call site when the level is disabled.
void log_message(LogLevel level, std::string_view msg,
                 std::source_location loc = std::source_location::current());

}  // namespace mcp

// clang-format off
#define MCP_LOG(level, msg)                                                    \
    do {                                                                       \
        if (::mcp::log_level() <= (level)) {                                   \
            ::mcp::log_message((level), (msg));                                \
        }                                                                      \
    } while (0)

#define MCP_LOG_TRACE(msg) MCP_LOG(::mcp::LogLevel::trace, msg)
#define MCP_LOG_DEBUG(msg) MCP_LOG(::mcp::LogLevel::debug, msg)
#define MCP_LOG_INFO(msg)  MCP_LOG(::mcp::LogLevel::info,  msg)
#define MCP_LOG_WARN(msg)  MCP_LOG(::mcp::LogLevel::warn,  msg)
#define MCP_LOG_ERROR(msg) MCP_LOG(::mcp::LogLevel::error, msg)
// clang-format on
