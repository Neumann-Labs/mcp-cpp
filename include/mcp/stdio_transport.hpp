// SPDX-License-Identifier: Apache-2.0
//
// POSIX-flavoured stdio transport for MCP.
//
// Reads newline-delimited JSON-RPC frames from a "read" file descriptor
// (default: stdin / FD 0) and writes them to a "write" file descriptor
// (default: stdout / FD 1). Per the MCP spec, frames MUST NOT contain
// embedded newlines, and stdout MUST NOT carry anything other than valid
// MCP frames — diagnostics belong on stderr.
//
// The read loop uses `poll(2)` so that `close()` can interrupt a read in
// progress via a self-pipe wake-up; the legacy implementation got this
// wrong and deadlocked on shutdown.

#pragma once

#if defined(_WIN32)
#error \
    "mcp::StdioTransport is POSIX-only (poll(2) + file descriptors). " \
    "On Windows, use the Streamable HTTP transport, or contribute a " \
    "Win32 stdio transport (overlapped I/O on stdin/stdout handles)."
#endif

#include "mcp/transport.hpp"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>

namespace mcp {

class StdioTransport final : public Transport {
public:
    struct Options {
        /// File descriptor to read from (default: STDIN_FILENO).
        int read_fd = 0;
        /// File descriptor to write to (default: STDOUT_FILENO).
        int write_fd = 1;
        /// Maximum size of a single frame the parser will accept. Frames
        /// exceeding this size cause a transport error and disconnect.
        std::size_t max_frame_bytes = 16 * 1024 * 1024;
        /// Whether the transport owns and should `close()` the file
        /// descriptors on destruction. Stdin/stdout are the typical case
        /// (false). Pipe-based test transports usually want true.
        bool owns_fds = false;
    };

    StdioTransport();
    explicit StdioTransport(Options opts);
    ~StdioTransport() override;

    void on_message(MessageCallback cb) override;
    void on_error(ErrorCallback cb)     override;
    void on_close(CloseCallback cb)     override;

    void start() override;
    void close() override;

    [[nodiscard]] std::error_code send(std::string_view frame) override;
    [[nodiscard]] bool is_open() const noexcept override {
        return running_.load(std::memory_order_acquire);
    }

private:
    void read_loop() noexcept;
    void deliver_error(std::error_code ec) noexcept;
    void fire_close() noexcept;

    Options opts_;

    int  wakeup_read_fd_  = -1;   // self-pipe read end (closed-on-stop)
    int  wakeup_write_fd_ = -1;

    std::atomic<bool> running_{false};
    std::atomic<bool> closing_{false};
    std::atomic<bool> close_fired_{false};

    std::thread read_thread_;
    std::mutex  write_mutex_;     // serializes writes to write_fd_

    // Callbacks. Set once before start(); read without locking thereafter.
    MessageCallback on_message_;
    ErrorCallback   on_error_;
    CloseCallback   on_close_;
};

}  // namespace mcp
