// SPDX-License-Identifier: Apache-2.0
#include "mcp/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace mcp {

namespace {

// Atomic level lets readers fast-path without grabbing the sink mutex.
std::atomic<LogLevel> g_level{LogLevel::info};

// The sink itself is mutex-guarded; swaps are rare, calls are frequent.
std::mutex   g_sink_mutex;
LogSink      g_sink;  // empty until first call to ensure_default_sink().

void default_sink(LogLevel level, std::string_view msg) {
    // Compose the line under a single mutex so multi-thread interleaving
    // doesn't garble output. fwrite is line-buffered on stderr by default.
    static std::mutex io_mutex;
    std::lock_guard<std::mutex> lk(io_mutex);

    using clock = std::chrono::system_clock;
    const auto now    = clock::now();
    const auto secs   = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            now - secs).count();
    const std::time_t tt = clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif

    std::fprintf(stderr,
                 "[%04d-%02d-%02d %02d:%02d:%02d.%06lld %-5s] %.*s\n",
                 tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                 tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                 static_cast<long long>(micros),
                 to_string(level).data(),
                 static_cast<int>(msg.size()), msg.data());
    std::fflush(stderr);
}

}  // namespace

std::string_view to_string(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::trace: return "TRACE";
        case LogLevel::debug: return "DEBUG";
        case LogLevel::info:  return "INFO";
        case LogLevel::warn:  return "WARN";
        case LogLevel::error: return "ERROR";
        case LogLevel::off:   return "OFF";
    }
    return "?";
}

void set_log_sink(LogSink sink) {
    std::lock_guard<std::mutex> lk(g_sink_mutex);
    g_sink = sink ? std::move(sink) : LogSink{&default_sink};
}

void set_log_level(LogLevel level) noexcept {
    g_level.store(level, std::memory_order_relaxed);
}

LogLevel log_level() noexcept {
    return g_level.load(std::memory_order_relaxed);
}

void log_message(LogLevel level, std::string_view msg,
                 std::source_location /*loc*/) {
    if (level < g_level.load(std::memory_order_relaxed)) return;
    LogSink sink;
    {
        std::lock_guard<std::mutex> lk(g_sink_mutex);
        if (!g_sink) g_sink = &default_sink;
        sink = g_sink;
    }
    sink(level, msg);
}

}  // namespace mcp
