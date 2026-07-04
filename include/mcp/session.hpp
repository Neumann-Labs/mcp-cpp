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
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

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

    /// Remove a previously-registered request handler. After this returns,
    /// inbound requests with `method` route to the fallback handler (or
    /// produce method_not_found if no fallback is set).
    void clear_request_handler(const std::string& method);

    /// Remove a previously-registered notification handler.
    void clear_notification_handler(const std::string& method);

    /// Fallback handler invoked when no method-specific request handler
    /// matches. The default rejects with method_not_found.
    void set_fallback_request_handler(RequestHandler h);

    /// Fallback handler for unmatched notifications; the default ignores.
    void set_fallback_notification_handler(NotificationHandler h);

    /// Hook fired when the underlying transport closes (EOF, error, or
    /// after a local close()). Fires at most once. Safe to register
    /// before or after start().
    using ClosedCallback = std::function<void()>;
    void set_on_closed(ClosedCallback cb);

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

    /// Send a JSON-RPC request and return a future for a *typed* result.
    /// `convert` turns the raw result JSON into `T`; it runs on the
    /// Session's read thread when the response arrives, so the caller's
    /// future resolves directly — no per-call worker thread. Any exception
    /// `convert` throws (e.g. a from_json type error) surfaces through the
    /// returned future's `.get()`. `.wait_for()` / `.wait_until()` behave
    /// normally (the future is backed by a real promise resolved in the
    /// background).
    template <class T>
    [[nodiscard]] std::future<T>
    send_request_for(std::string               method,
                     nlohmann::json            params,
                     std::function<T(nlohmann::json)> convert,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) {
        auto pr  = std::make_shared<std::promise<T>>();
        auto fut = pr->get_future();
        Pending pending;
        pending.resolve =
            [pr, convert = std::move(convert)](nlohmann::json j) {
                try {
                    pr->set_value(convert(std::move(j)));
                } catch (...) {
                    pr->set_exception(std::current_exception());
                }
            };
        pending.reject = [pr](std::exception_ptr e) { pr->set_exception(e); };
        register_pending(std::move(method), std::move(params),
                         std::move(pending), timeout);
        return fut;
    }

    /// Like `send_request_for`, but for methods whose result carries no
    /// payload the caller cares about (`ping`, `subscribe`, ...). The
    /// future resolves to void on success and throws on error.
    [[nodiscard]] std::future<void>
    send_request_void(std::string               method,
                      nlohmann::json            params  = nullptr,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

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

    /// A parked outbound request. Instead of a bare promise we hold
    /// type-erased resolvers so the typed `send_request_for<T>` can run
    /// its json->T conversion at resolution time. Exactly one of
    /// `resolve` / `reject` is invoked, always *outside* `pending_mu_`
    /// (resolving a future can wake a waiter that re-enters the Session).
    struct Pending {
        std::function<void(nlohmann::json)>     resolve;
        std::function<void(std::exception_ptr)> reject;
        std::chrono::steady_clock::time_point   deadline;
    };

    /// Allocate an id, park `pending`, and send the request frame. On a
    /// closed session, duplicate id, or transport failure it invokes
    /// `pending.reject` (after releasing the lock) instead.
    void register_pending(std::string    method,
                          nlohmann::json params,
                          Pending        pending,
                          std::chrono::milliseconds timeout);

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

    std::mutex                                              closed_cb_mu_;
    ClosedCallback                                          on_closed_;
    std::atomic<bool>                                       closed_cb_fired_{false};

    // Tracks live request-dispatch workers so close() can wait for
    // them to finish before any member they touch (transport_,
    // handler functions) is destroyed.
    std::mutex                                              workers_mu_;
    std::condition_variable                                 workers_cv_;
    int                                                     workers_running_ = 0;
};

}  // namespace mcp
