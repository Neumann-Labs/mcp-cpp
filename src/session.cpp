// SPDX-License-Identifier: Apache-2.0
#include "mcp/session.hpp"

#include "mcp/error.hpp"
#include "mcp/log.hpp"
#include "mcp/protocol.hpp"
#include "mcp/transport.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace mcp {

using std::chrono::steady_clock;

// =====================================================================
// Construction / destruction
// =====================================================================

Session::Session(std::unique_ptr<Transport> transport)
    : Session(std::move(transport), Options{}) {}

Session::Session(std::unique_ptr<Transport> transport, Options opts)
    : transport_(std::move(transport)), opts_(opts) {
    if (!transport_) {
        throw Error{error_code::internal_error, "Session: transport is null"};
    }
    transport_->on_message([this](std::string raw) { this->handle_frame(std::move(raw)); });
    transport_->on_error([](std::error_code ec) {
        MCP_LOG_WARN(std::string{"transport error: "} + ec.message());
    });
    transport_->on_close([this]() {
        ErrorObject reason{
            error_code::internal_error,
            "transport closed",
            {},
        };
        cancel_all_pending(reason);
        // Mark the session closed so is_open() flips, and notify any
        // user code waiting on the close hook (e.g. Server::run).
        closed_.store(true, std::memory_order_release);
        ClosedCallback cb;
        {
            std::lock_guard<std::mutex> lk(closed_cb_mu_);
            if (!closed_cb_fired_.exchange(true, std::memory_order_acq_rel)) {
                cb = on_closed_;
            }
        }
        if (cb) {
            try { cb(); }
            catch (const std::exception& e) {
                MCP_LOG_ERROR(std::string{"on_closed threw: "} + e.what());
            }
        }
    });
}

Session::~Session() {
    close();
}

// =====================================================================
// Handlers
// =====================================================================

void Session::set_request_handler(std::string method, RequestHandler h) {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    req_handlers_[std::move(method)] = std::move(h);
}

void Session::set_notification_handler(std::string method, NotificationHandler h) {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    note_handlers_[std::move(method)] = std::move(h);
}

void Session::set_fallback_request_handler(RequestHandler h) {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    fallback_req_ = std::move(h);
}

void Session::set_fallback_notification_handler(NotificationHandler h) {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    fallback_note_ = std::move(h);
}

void Session::set_on_closed(ClosedCallback cb) {
    std::lock_guard<std::mutex> lk(closed_cb_mu_);
    on_closed_ = std::move(cb);
}

// =====================================================================
// Lifecycle
// =====================================================================

void Session::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        return;
    }
    closed_.store(false, std::memory_order_release);
    timeout_thread_ = std::thread([this] { this->timeout_loop(); });
    transport_->start();
}

void Session::close() {
    // Two callers can land here: (1) the application calling close()
    // directly, (2) the Session destructor. Whichever arrives first
    // tears down the transport; both must run the rest so that
    // double-destruction can't leave the timeout thread joinable
    // (which would call std::terminate from ~thread).
    bool expected = false;
    const bool first = closed_.compare_exchange_strong(expected, true,
                                                       std::memory_order_acq_rel);
    if (first && transport_) transport_->close();

    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        timeout_cv_.notify_all();
    }
    if (timeout_thread_.joinable()) timeout_thread_.join();

    cancel_all_pending(ErrorObject{
        error_code::internal_error, "session closed", {},
    });

    // Wait for any in-flight request-dispatch workers to finish so
    // they don't reach into destroyed members after we return.
    {
        std::unique_lock<std::mutex> lk(workers_mu_);
        workers_cv_.wait(lk, [this] { return workers_running_ == 0; });
    }
}

// =====================================================================
// Send paths
// =====================================================================

RequestId Session::next_id() noexcept {
    return RequestId{next_id_.fetch_add(1, std::memory_order_relaxed)};
}

std::future<nlohmann::json>
Session::send_request(std::string method,
                      nlohmann::json params,
                      std::chrono::milliseconds timeout) {
    auto id = next_id();

    JsonRpcRequest req;
    req.id     = id;
    req.method = std::move(method);
    if (!params.is_null()) req.params = std::move(params);

    const auto effective_timeout =
        timeout == std::chrono::milliseconds{0} ? opts_.default_request_timeout : timeout;
    const auto deadline = steady_clock::now() + effective_timeout;

    Pending pending;
    pending.deadline = deadline;
    auto future = pending.promise.get_future();

    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_.emplace(id, std::move(pending));
        timeout_cv_.notify_all();
    }

    try {
        send_message(req);
    } catch (...) {
        // If the transport refused, drop the pending entry and surface the
        // error through the future so the caller observes it on .get().
        std::promise<nlohmann::json> p;
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            auto it = pending_.find(id);
            if (it != pending_.end()) {
                p = std::move(it->second.promise);
                pending_.erase(it);
            }
        }
        try {
            std::rethrow_exception(std::current_exception());
        } catch (...) {
            p.set_exception(std::current_exception());
        }
    }

    return future;
}

std::error_code
Session::send_notification(std::string method, nlohmann::json params) {
    JsonRpcNotification note;
    note.method = std::move(method);
    if (!params.is_null()) note.params = std::move(params);
    try {
        send_message(note);
    } catch (const std::system_error& e) {
        return e.code();
    } catch (...) {
        return std::make_error_code(std::errc::io_error);
    }
    return {};
}

void Session::send_message(const JsonRpcMessage& msg) {
    const auto j   = serialize_message(msg);
    const auto str = j.dump();
    auto ec = transport_->send(str);
    if (ec) {
        throw std::system_error{ec, "Session::send_message: transport->send failed"};
    }
}

// =====================================================================
// Inbound dispatch
// =====================================================================

void Session::handle_frame(std::string raw) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(raw);
    } catch (const nlohmann::json::exception& e) {
        MCP_LOG_WARN(std::string{"rejecting non-JSON frame: "} + e.what());
        return;  // JSON-RPC says we send a parse error response, but per the
                 // spec we cannot when there is no id; simplest is to drop.
    }

    JsonRpcMessage msg;
    try {
        msg = parse_message(j);
    } catch (const Error& e) {
        MCP_LOG_WARN(std::string{"rejecting malformed JSON-RPC frame: "} + e.what());
        return;
    }

    std::visit([&](auto&& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, JsonRpcRequest>) {
            // Dispatch requests to a detached worker so user handlers
            // can themselves issue further requests on this Session
            // (e.g. server.sample() inside a tool handler) without
            // deadlocking the read thread.
            //
            // close() waits for `workers_running_` to drain before
            // returning so the dispatched worker can never outlive
            // members it touches (transport_, handlers).
            {
                std::lock_guard<std::mutex> lk(workers_mu_);
                ++workers_running_;
            }
            std::thread([this, req = std::move(m)]() mutable {
                handle_request(std::move(req));
                {
                    std::lock_guard<std::mutex> lk(workers_mu_);
                    --workers_running_;
                }
                workers_cv_.notify_all();
            }).detach();
        } else if constexpr (std::is_same_v<T, JsonRpcNotification>) {
            handle_notification(std::move(m));
        } else {
            handle_response(std::move(m));
        }
    }, msg);
}

void Session::handle_request(JsonRpcRequest req) {
    RequestHandler handler;
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        auto it = req_handlers_.find(req.method);
        if (it != req_handlers_.end()) handler = it->second;
        else                            handler = fallback_req_;
    }

    JsonRpcResponse resp;
    resp.id = req.id;

    if (!handler) {
        resp.outcome = ErrorObject{
            error_code::method_not_found,
            "method not found: " + req.method,
            {},
        };
    } else {
        try {
            const auto& params_in = req.params.value_or(nullptr);
            resp.outcome = handler(params_in);
        } catch (const Error& e) {
            resp.outcome = e.object();
        } catch (const std::exception& e) {
            resp.outcome = ErrorObject{
                error_code::internal_error,
                std::string{"handler threw: "} + e.what(),
                {},
            };
        } catch (...) {
            resp.outcome = ErrorObject{
                error_code::internal_error,
                "handler threw a non-std exception",
                {},
            };
        }
    }

    try {
        send_message(resp);
    } catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"failed to send response: "} + e.what());
    }
}

void Session::handle_notification(JsonRpcNotification note) {
    NotificationHandler handler;
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        auto it = note_handlers_.find(note.method);
        if (it != note_handlers_.end()) handler = it->second;
        else                             handler = fallback_note_;
    }
    if (!handler) return;
    try {
        handler(note.params.value_or(nullptr));
    } catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"notification handler threw: "} + e.what());
    } catch (...) {
        MCP_LOG_ERROR("notification handler threw a non-std exception");
    }
}

void Session::handle_response(JsonRpcResponse resp) {
    if (!resp.id.has_value()) {
        // Per spec, an error response with no id can occur on parse errors
        // we issued; nothing to correlate against.
        if (resp.is_error()) {
            MCP_LOG_WARN(std::string{"received untargeted error: "} + resp.error().message);
        }
        return;
    }

    Pending pending;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        auto it = pending_.find(*resp.id);
        if (it == pending_.end()) {
            MCP_LOG_WARN("received response for unknown id: " + resp.id->canonical());
            return;
        }
        pending = std::move(it->second);
        pending_.erase(it);
        timeout_cv_.notify_all();
    }

    if (resp.is_success()) {
        pending.promise.set_value(std::move(resp).result());
    } else {
        try {
            throw Error{resp.error()};
        } catch (...) {
            pending.promise.set_exception(std::current_exception());
        }
    }
}

// =====================================================================
// Timeout
// =====================================================================

void Session::timeout_loop() {
    std::unique_lock<std::mutex> lk(pending_mu_);
    while (!closed_.load(std::memory_order_acquire)) {
        if (pending_.empty()) {
            timeout_cv_.wait(lk);
            continue;
        }

        auto soonest = steady_clock::time_point::max();
        for (const auto& [id, p] : pending_) {
            if (p.deadline < soonest) soonest = p.deadline;
        }
        if (timeout_cv_.wait_until(lk, soonest) == std::cv_status::timeout) {
            const auto now = steady_clock::now();
            // Sweep expired requests.
            for (auto it = pending_.begin(); it != pending_.end(); ) {
                if (it->second.deadline <= now) {
                    auto promise = std::move(it->second.promise);
                    it = pending_.erase(it);
                    lk.unlock();
                    try {
                        throw Error{error_code::internal_error,
                                    "request timed out"};
                    } catch (...) {
                        promise.set_exception(std::current_exception());
                    }
                    lk.lock();
                } else {
                    ++it;
                }
            }
        }
    }
}

void Session::cancel_all_pending(const ErrorObject& reason) {
    std::unordered_map<RequestId, Pending> drained;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        drained.swap(pending_);
        timeout_cv_.notify_all();
    }
    for (auto& [id, p] : drained) {
        try {
            throw Error{reason};
        } catch (...) {
            p.promise.set_exception(std::current_exception());
        }
    }
}

}  // namespace mcp
