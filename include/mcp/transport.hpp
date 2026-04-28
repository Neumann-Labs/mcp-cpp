// SPDX-License-Identifier: Apache-2.0
//
// Abstract transport interface.
//
// A transport shovels already-serialized JSON-RPC frames between two peers.
// It does not parse or validate — that is the Session's job. Frames sent
// upward are complete UTF-8 JSON objects (one per call to the message
// callback). Frames sent downward are likewise complete JSON; the transport
// adds whatever framing its protocol requires (newlines for stdio,
// chunked SSE / one-shot HTTP for streamable HTTP, etc.).
//
// Threading: callbacks may be invoked from a transport-owned thread.
// `send()` is safe to call from any thread.

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <system_error>

namespace mcp {

class Transport {
public:
    using MessageCallback = std::function<void(std::string)>;
    using ErrorCallback   = std::function<void(std::error_code)>;
    using CloseCallback   = std::function<void()>;

    virtual ~Transport() = default;

    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;

    /// Set the inbound-frame callback. Must be set before `start()`.
    virtual void on_message(MessageCallback cb) = 0;

    /// Set the error callback. Optional; defaults to a no-op.
    virtual void on_error(ErrorCallback cb) = 0;

    /// Set the close callback, fired once when the transport observes the
    /// peer has gone away (EOF, socket close, etc.) or after a local
    /// `close()` has fully wound down. Optional.
    virtual void on_close(CloseCallback cb) = 0;

    /// Begin processing. The transport may spawn one or more threads.
    /// Idempotent: calling `start()` after `start()` is a no-op.
    virtual void start() = 0;

    /// Stop processing. Blocks until the worker thread(s) have exited.
    /// Idempotent: calling `close()` more than once is a no-op.
    virtual void close() = 0;

    /// Send a single complete JSON-RPC frame. Returns an empty error_code
    /// on success. Thread-safe.
    [[nodiscard]] virtual std::error_code send(std::string_view frame) = 0;

    /// Whether the transport is currently running.
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
};

}  // namespace mcp
