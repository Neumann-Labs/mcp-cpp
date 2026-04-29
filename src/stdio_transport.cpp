// SPDX-License-Identifier: Apache-2.0
#include "mcp/stdio_transport.hpp"

#include "mcp/log.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

namespace mcp {

namespace {

constexpr std::size_t kReadChunkBytes = 8 * 1024;

// A small POSIX-flavored helper that turns errno into an std::error_code.
inline std::error_code last_errno_ec() noexcept {
    return std::error_code{errno, std::generic_category()};
}

// Set FD_CLOEXEC + O_NONBLOCK on a pipe end. Returns the resulting fd or
// -1 on failure (errno preserved).
int make_nonblocking(int fd) noexcept {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags < 0) return -1;
    if (fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0) return -1;
    return fd;
}

// Close an fd ignoring EINTR.
void quiet_close(int fd) noexcept {
    if (fd < 0) return;
    while (::close(fd) < 0) {
        if (errno != EINTR) break;
    }
}

}  // namespace

StdioTransport::StdioTransport() : StdioTransport(Options{}) {}

StdioTransport::StdioTransport(Options opts) : opts_(opts) {}

StdioTransport::~StdioTransport() {
    close();
    if (opts_.owns_fds) {
        quiet_close(opts_.read_fd);
        // Avoid double-closing a write_fd that aliases read_fd.
        if (opts_.write_fd != opts_.read_fd) quiet_close(opts_.write_fd);
    }
}

void StdioTransport::on_message(MessageCallback cb) { on_message_ = std::move(cb); }
void StdioTransport::on_error(ErrorCallback cb)     { on_error_   = std::move(cb); }
void StdioTransport::on_close(CloseCallback cb)     { on_close_   = std::move(cb); }

void StdioTransport::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        return;  // already running
    }
    closing_.store(false, std::memory_order_release);
    close_fired_.store(false, std::memory_order_release);

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        running_.store(false, std::memory_order_release);
        deliver_error(last_errno_ec());
        return;
    }
    if (make_nonblocking(pipefd[0]) < 0 || make_nonblocking(pipefd[1]) < 0) {
        const auto ec = last_errno_ec();
        quiet_close(pipefd[0]);
        quiet_close(pipefd[1]);
        running_.store(false, std::memory_order_release);
        deliver_error(ec);
        return;
    }
    wakeup_read_fd_  = pipefd[0];
    wakeup_write_fd_ = pipefd[1];

    read_thread_ = std::thread([this]() noexcept { this->read_loop(); });
}

void StdioTransport::close() {
    // Only the first caller to flip `closing_` does the wake + cleanup. All
    // callers wait for the read thread to finish before returning, so the
    // destructor's contract ("on return, the worker is gone") holds even
    // when EOF or an error has already retired the loop.
    const bool first = !closing_.exchange(true, std::memory_order_acq_rel);
    running_.store(false, std::memory_order_release);

    if (first && wakeup_write_fd_ >= 0) {
        const char b = 0;
        for (;;) {
            ssize_t n = ::write(wakeup_write_fd_, &b, 1);
            if (n == 1)             break;
            if (n < 0 && errno == EINTR) continue;
            break;  // EAGAIN: the wake pipe is full, which still wakes the reader.
        }
    }

    if (read_thread_.joinable()) read_thread_.join();

    if (first) {
        quiet_close(wakeup_read_fd_);
        quiet_close(wakeup_write_fd_);
        wakeup_read_fd_  = -1;
        wakeup_write_fd_ = -1;
    }

    fire_close();
}

std::error_code StdioTransport::send(std::string_view frame) {
    if (!running_.load(std::memory_order_acquire)) {
        return std::make_error_code(std::errc::not_connected);
    }
    if (frame.find('\n') != std::string_view::npos) {
        // Per spec: frames MUST NOT contain embedded newlines.
        return std::make_error_code(std::errc::invalid_argument);
    }

    std::lock_guard<std::mutex> lk(write_mutex_);
    const char* data    = frame.data();
    std::size_t left    = frame.size();
    while (left > 0) {
        ssize_t n = ::write(opts_.write_fd, data, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return last_errno_ec();
        }
        data += n;
        left -= static_cast<std::size_t>(n);
    }

    static const char nl = '\n';
    while (true) {
        ssize_t n = ::write(opts_.write_fd, &nl, 1);
        if (n == 1) break;
        if (n < 0 && errno == EINTR) continue;
        return last_errno_ec();
    }
    return {};
}

void StdioTransport::read_loop() noexcept {
    std::string  buffer;
    buffer.reserve(kReadChunkBytes);

    char chunk[kReadChunkBytes];

    while (running_.load(std::memory_order_acquire)) {
        struct pollfd fds[2];
        fds[0].fd     = opts_.read_fd;
        fds[0].events = POLLIN;
        fds[1].fd     = wakeup_read_fd_;
        fds[1].events = POLLIN;

        int pr = ::poll(fds, 2, -1 /* infinite */);
        if (pr < 0) {
            if (errno == EINTR) continue;
            deliver_error(last_errno_ec());
            break;
        }

        if (fds[1].revents & POLLIN) {
            // Wake-up; either we're closing or the pipe has stale bytes.
            char drain[64];
            while (::read(wakeup_read_fd_, drain, sizeof(drain)) > 0) { /* drain */ }
            if (closing_.load(std::memory_order_acquire)) break;
        }

        if (fds[0].revents & (POLLERR | POLLNVAL)) {
            deliver_error(std::make_error_code(std::errc::io_error));
            break;
        }

        if (fds[0].revents & POLLIN) {
            ssize_t n = ::read(opts_.read_fd, chunk, sizeof(chunk));
            if (n == 0) {
                // EOF on the input.
                break;
            }
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                deliver_error(last_errno_ec());
                break;
            }

            for (ssize_t i = 0; i < n; ++i) {
                const char c = chunk[i];
                if (c == '\n') {
                    // Tolerate CRLF: peers writing through Windows-style
                    // line endings get their trailing \r stripped before
                    // we hand the buffer up. Per MCP spec the delimiter
                    // is '\n', so this keeps interop with peers that
                    // happen to ship CRLF without a fight.
                    if (!buffer.empty() && buffer.back() == '\r') {
                        buffer.pop_back();
                    }
                    if (!buffer.empty() && on_message_) {
                        try {
                            on_message_(std::move(buffer));
                        } catch (const std::exception& e) {
                            MCP_LOG_ERROR(std::string{"on_message threw: "} + e.what());
                        } catch (...) {
                            MCP_LOG_ERROR("on_message threw a non-std exception");
                        }
                    }
                    buffer.clear();
                    buffer.reserve(kReadChunkBytes);
                    continue;
                }
                if (buffer.size() >= opts_.max_frame_bytes) {
                    deliver_error(std::make_error_code(std::errc::message_size));
                    return fire_close();
                }
                buffer.push_back(c);
            }
        } else if (fds[0].revents & POLLHUP) {
            break;  // peer closed
        }
    }

    fire_close();
}

void StdioTransport::deliver_error(std::error_code ec) noexcept {
    if (!on_error_) return;
    try {
        on_error_(ec);
    } catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"on_error threw: "} + e.what());
    } catch (...) {
        MCP_LOG_ERROR("on_error threw a non-std exception");
    }
}

void StdioTransport::fire_close() noexcept {
    bool already = close_fired_.exchange(true, std::memory_order_acq_rel);
    if (already) return;
    running_.store(false, std::memory_order_release);
    if (!on_close_) return;
    try {
        on_close_();
    } catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"on_close threw: "} + e.what());
    } catch (...) {
        MCP_LOG_ERROR("on_close threw a non-std exception");
    }
}

}  // namespace mcp
