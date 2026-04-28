// SPDX-License-Identifier: Apache-2.0
//
// Session: the JSON-RPC dispatch layer that sits between a Transport and
// the Server / Client / application code.
//
// Responsibilities:
//   - Parse inbound raw frames into JsonRpcMessage and route them.
//   - Correlate outbound requests with inbound responses by id, returning
//     a std::future<nlohmann::json> that resolves with the result or
//     throws mcp::Error on failure.
//   - Generate fresh, monotonically-increasing request ids.
//   - Apply per-request timeouts; an expired request raises Error with
//     code internal_error and a "request timed out" message.
//   - Dispatch inbound requests and notifications to method-keyed
//     handlers registered by the application.
//   - Tear down cleanly: cancel every pending future with a transport
//     error, then close the underlying transport.
//
// Threading: handler invocations happen on the transport's read thread.
// Application handlers SHOULD NOT block for long; if they do, queue the
// work elsewhere.
//
// One Session instance is sufficient for either side of the connection;
// `Server` and `Client` both compose a Session rather than inheriting.

#pragma once

#include "mcp/error.hpp"
#include "mcp/protocol.hpp"
#include "mcp/transport.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace mcp {

class Session {
public:
    /// Handler invoked when a peer sends a request with `method`.
    /// Returns the result JSON (which is wrapped into a JSON-RPC response).
    /// Throwing `mcp::Error` reports back to the peer as a JSON-RPC error.
    /// Throwing anything else is reported as InternalError.
    using RequestHandler = std::function<nlohmann::json(const nlohmann::json& params)>;

    /// Handler invoked when a peer sends a notification with `method`.
    /// Exceptions are caught and logged.
    using NotificationHandler = std::function<void(const nlohmann::json& params)>;

    struct Options {
        std::chrono::milliseconds default_request_timeout{30 * 1000};  // 30s
    };

    explicit Session(std::unique_ptr<Transport> transport);
    Session(std::unique_ptr<Transport> transport, Options opts);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    /// Register a handler for inbound requests with the given method.
    /// Replaces any previous handler.  Must be called before `start()`
    /// for races against the first frame to be impossible; calling later
    /// is also safe but the first matching frame may have already been
    /// rejected as method-not-found.
    void set_request_handler(std::string method, RequestHandler h);

    /// Register a handler for inbound notifications with the given method.
    void set_notification_handler(std::string method, NotificationHandler h);

    /// Fallback handler invoked when no method-specific request handler
    /// matches. The default rejects with method_not_found.
    void set_fallback_request_handler(RequestHandler h);

    /// Fallback handler for unmatched notifications; the default ignores.
    void set_fallback_notification_handler(NotificationHandler h);

    /// Begin processing. Idempotent.
    void start();

    /// Stop processing, cancel all pending requests, and close the
    /// transport. Idempotent.
    void close();

    /// Send a JSON-RPC request and return a future for its result.
    /// `timeout` of zero means use `Options::default_request_timeout`.
    [[nodiscard]] std::future<nlohmann::json>
    send_request(std::string                method,
                 nlohmann::json             params  = nullptr,
                 std::chrono::milliseconds  timeout = std::chrono::milliseconds{0});

    /// Send a JSON-RPC notification (fire-and-forget).
    std::error_code send_notification(std::string    method,
                                      nlohmann::json params = nullptr);

    [[nodiscard]] bool is_open() const noexcept {
        return started_.load(std::memory_order_acquire) &&
               !closed_.load(std::memory_order_acquire);
    }

private:
    void handle_frame(std::string raw);
    void handle_request(JsonRpcRequest req);
    void handle_notification(JsonRpcNotification note);
    void handle_response(JsonRpcResponse resp);
    void send_message(const JsonRpcMessage& msg);
    void timeout_loop();
    void cancel_all_pending(const ErrorObject& reason);

    [[nodiscard]] RequestId next_id() noexcept;

    struct Pending {
        std::promise<nlohmann::json>           promise;
        std::chrono::steady_clock::time_point  deadline;
    };

    std::unique_ptr<Transport> transport_;
    Options                    opts_;

    std::atomic<bool>          started_{false};
    std::atomic<bool>          closed_{false};
    std::atomic<std::int64_t>  next_id_{1};

    // Pending outbound requests awaiting a response. Keyed by canonical
    // RequestId string so int and string ids never collide.
    std::mutex                                              pending_mu_;
    std::unordered_map<RequestId, Pending>                  pending_;
    std::condition_variable                                 timeout_cv_;
    std::thread                                             timeout_thread_;

    // Handler tables. Read-mostly; protected by handlers_mu_ so they can
    // be modified after start() without UB.
    std::mutex                                              handlers_mu_;
    std::unordered_map<std::string, RequestHandler>         req_handlers_;
    std::unordered_map<std::string, NotificationHandler>    note_handlers_;
    RequestHandler                                          fallback_req_;
    NotificationHandler                                     fallback_note_;
};

}  // namespace mcp
