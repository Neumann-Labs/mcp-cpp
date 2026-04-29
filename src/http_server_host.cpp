// SPDX-License-Identifier: Apache-2.0
#include "mcp/http_server_host.hpp"

#include "mcp/error.hpp"
#include "mcp/log.hpp"
#include "mcp/protocol.hpp"
#include "mcp/session.hpp"
#include "mcp/server.hpp"
#include "mcp/transport.hpp"

#include <httplib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mcp {

namespace {

// =====================================================================
// Per-session HTTP transport adapter
// =====================================================================
//
// One of these lives inside each per-session SessionContext. When a
// POST arrives carrying a JSON-RPC *request*, the host enqueues a
// pending response slot keyed by the request id, hands the body to
// `deliver`, and awaits the slot's future. The Session dispatches the
// request to a worker, the user's handler returns, the Session calls
// `send(response_frame)`, and that send routes to the matching slot
// to satisfy the future. The POST handler then writes the JSON to
// the HTTP response.
//
// Notifications and responses inbound from the peer get fed via
// `deliver` and produce no future; the POST handler returns 202.
//
// Server-initiated traffic (transport->send called with no matching
// pending slot) is currently dropped with a warning. Phase 3e/3 adds
// a GET-opened SSE stream to receive that traffic.

class HttpSessionTransport final : public Transport {
public:
    void on_message(MessageCallback cb) override {
        std::deque<std::string> drain;
        {
            std::lock_guard<std::mutex> lk(buffered_mu_);
            on_message_ = std::move(cb);
            buffered_.swap(drain);
        }
        // Replay frames that arrived before the callback was set.
        // This races with HttpServerHost::start() spawning a session
        // thread: the host's POST handler can call deliver() the
        // moment the SessionContext is in the map, but Server::run on
        // the new thread hasn't yet wired on_message via the Session
        // constructor. We park frames here so nothing is silently
        // lost.
        if (on_message_) {
            for (auto& f : drain) on_message_(std::move(f));
        }
    }
    void on_error(ErrorCallback cb)     override { on_error_   = std::move(cb); }
    void on_close(CloseCallback cb)     override { on_close_   = std::move(cb); }

    void start() override {
        running_.store(true, std::memory_order_release);
    }

    void close() override {
        if (closed_.exchange(true, std::memory_order_acq_rel)) return;
        running_.store(false, std::memory_order_release);
        // Cancel any waiters on pending POST replies.
        std::unordered_map<std::string, std::promise<std::string>> drained;
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            drained.swap(pending_);
        }
        for (auto& [id, p] : drained) {
            try { throw Error{error_code::internal_error, "session closed"}; }
            catch (...) { p.set_exception(std::current_exception()); }
        }
        if (on_close_) {
            try { on_close_(); }
            catch (const std::exception& e) {
                MCP_LOG_ERROR(std::string{"HttpSessionTransport on_close threw: "} + e.what());
            } catch (...) {
                MCP_LOG_ERROR("HttpSessionTransport on_close threw a non-std exception");
            }
        }
    }

    [[nodiscard]] std::error_code send(std::string_view frame) override {
        if (!running_.load(std::memory_order_acquire) ||
             closed_.load(std::memory_order_acquire)) {
            return std::make_error_code(std::errc::not_connected);
        }
        // Parse the frame's id (responses have one) so we can route
        // back to the awaiting POST slot.
        nlohmann::json j;
        try { j = nlohmann::json::parse(frame); }
        catch (const std::exception& e) {
            MCP_LOG_ERROR(std::string{"HttpSessionTransport: bad outbound JSON: "} + e.what());
            return std::make_error_code(std::errc::invalid_argument);
        }
        const bool has_id     = j.contains("id") && !j["id"].is_null();
        const bool has_method = j.contains("method");

        if (has_id && !has_method) {
            // Response — route to the awaiting POST.
            std::string key = canonical_id(j["id"]);
            std::promise<std::string> awaiter;
            {
                std::lock_guard<std::mutex> lk(pending_mu_);
                auto it = pending_.find(key);
                if (it == pending_.end()) {
                    MCP_LOG_WARN("HttpSessionTransport: no awaiter for response id="
                                 + key);
                    return std::make_error_code(std::errc::invalid_argument);
                }
                awaiter = std::move(it->second);
                pending_.erase(it);
            }
            awaiter.set_value(std::string{frame});
            return {};
        }

        // Notification or server-initiated request. v1 has no GET
        // stream; drop with a warning for now.
        MCP_LOG_WARN("HttpSessionTransport: server-initiated frame dropped "
                     "(GET stream not implemented in this build)");
        return {};
    }

    [[nodiscard]] bool is_open() const noexcept override {
        return running_.load(std::memory_order_acquire) &&
              !closed_.load(std::memory_order_acquire);
    }

    // Host-facing helpers ----------------------------------------------------

    /// Hand a frame received from the peer up to the Session. If the
    /// frame is a *request*, returns a future that resolves to the
    /// outbound response string. If it's a notification or response,
    /// returns std::nullopt (POST should reply 202).
    std::optional<std::future<std::string>>
    deliver_request_or_notification(std::string raw) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(raw); }
        catch (...) { return std::nullopt; }
        const bool is_request = j.contains("id") && !j["id"].is_null()
                             && j.contains("method");

        std::optional<std::future<std::string>> reply;
        if (is_request) {
            std::string key = canonical_id(j["id"]);
            std::promise<std::string> p;
            reply = p.get_future();
            std::lock_guard<std::mutex> lk(pending_mu_);
            pending_.emplace(std::move(key), std::move(p));
        }
        // Deliver under buffered_mu_ so a concurrent on_message setter
        // either gets the frame replayed or fires on_message itself —
        // exactly one of the two paths runs the user callback.
        std::lock_guard<std::mutex> lk(buffered_mu_);
        if (on_message_) {
            on_message_(std::move(raw));
        } else {
            buffered_.emplace_back(std::move(raw));
        }
        return reply;
    }

private:
    static std::string canonical_id(const nlohmann::json& id) {
        if (id.is_string())          return "s:" + id.get<std::string>();
        if (id.is_number_integer())  return "i:" + std::to_string(id.get<std::int64_t>());
        if (id.is_number_unsigned()) return "i:" + std::to_string(id.get<std::int64_t>());
        return std::string{};
    }

    std::atomic<bool>                                            running_{false};
    std::atomic<bool>                                            closed_{false};

    // Pre-callback buffer + the callback itself live under the same
    // mutex, so the "is on_message_ set?" decision and the
    // corresponding action (call vs. enqueue) cannot race with the
    // setter.
    std::mutex                                                   buffered_mu_;
    MessageCallback                                              on_message_;
    std::deque<std::string>                                      buffered_;

    ErrorCallback                                                on_error_;
    CloseCallback                                                on_close_;

    std::mutex                                                   pending_mu_;
    std::unordered_map<std::string, std::promise<std::string>>   pending_;
};

// =====================================================================
// Adapter so a shared_ptr<HttpSessionTransport> can be handed to
// Server::run, which insists on std::unique_ptr<Transport>.
// =====================================================================

class SharedTransportShim final : public Transport {
public:
    explicit SharedTransportShim(std::shared_ptr<HttpSessionTransport> inner)
        : inner_(std::move(inner)) {}

    void on_message(MessageCallback cb) override { inner_->on_message(std::move(cb)); }
    void on_error(ErrorCallback cb)     override { inner_->on_error(std::move(cb));   }
    void on_close(CloseCallback cb)     override { inner_->on_close(std::move(cb));   }
    void start() override { inner_->start(); }
    void close() override { inner_->close(); }
    [[nodiscard]] std::error_code send(std::string_view f) override {
        return inner_->send(f);
    }
    [[nodiscard]] bool is_open() const noexcept override {
        return inner_->is_open();
    }

private:
    std::shared_ptr<HttpSessionTransport> inner_;
};

// =====================================================================
// Session-id minting
// =====================================================================

std::string make_session_id() {
    // 128-bit hex string from a per-call seeded RNG. Cryptographic
    // strength isn't promised here (std::random_device quality varies
    // per platform) — this is "unguessable enough" for non-adversarial
    // single-tenant hosts. Phase 3e/3 may upgrade to /dev/urandom.
    std::random_device rd;
    std::mt19937_64 rng{(static_cast<std::uint64_t>(rd()) << 32) ^ rd()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream os;
    os << std::hex << dist(rng) << dist(rng);
    return os.str();
}

}  // namespace

// =====================================================================
// Per-session context owned by the host
// =====================================================================

struct HttpServerHost::SessionContext {
    std::string                                  id;
    std::shared_ptr<HttpSessionTransport>        transport;
    std::unique_ptr<Server>                      server;
    std::thread                                  run_thread;
    std::chrono::steady_clock::time_point        last_seen;
};

// =====================================================================
// Host impl (httplib::Server lives here, and the session map)
// =====================================================================

struct HttpServerHost::Impl {
    httplib::Server                                              http;
    std::thread                                                  listener;
    int                                                          port = 0;

    std::mutex                                                   sessions_mu;
    std::unordered_map<std::string,
                       std::shared_ptr<HttpServerHost::SessionContext>>
                                                                 sessions;
};

// =====================================================================
// Construction / destruction
// =====================================================================

HttpServerHost::HttpServerHost(Implementation server_info,
                               Options        opts,
                               SessionFactory factory)
    : server_info_(std::move(server_info)),
      opts_(std::move(opts)),
      factory_(std::move(factory)),
      impl_(std::make_unique<Impl>()) {}

HttpServerHost::~HttpServerHost() { stop(); }

int HttpServerHost::port() const noexcept { return impl_ ? impl_->port : 0; }

std::size_t HttpServerHost::active_sessions() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lk(impl_->sessions_mu);
    return impl_->sessions.size();
}

// =====================================================================
// Origin validation
// =====================================================================

namespace {

bool origin_allowed(const httplib::Request& req,
                    const std::vector<std::string>& allow) {
    auto it = req.headers.find("Origin");
    if (it == req.headers.end()) return true;  // no Origin header -> not a browser-initiated XS request
    if (allow.empty()) return false;
    for (const auto& a : allow) {
        if (a == "*" || a == it->second) return true;
    }
    return false;
}

}  // namespace

// =====================================================================
// Lifecycle
// =====================================================================

void HttpServerHost::start() {
    if (started_.exchange(true, std::memory_order_acq_rel)) return;
    auto* host = this;

    // Capture small things so the lambdas don't copy the host.
    auto& srv = impl_->http;

    // Capture-by-value lambdas live for the lifetime of the
    // httplib::Server, which we own. start() returns after binding,
    // so any locals captured by reference here would dangle on every
    // subsequent request.

    // POST: client sends a frame. Reply with either application/json
    // (response to a request) or 202 Accepted (notification/response).
    srv.Post(opts_.path, [host](const httplib::Request& req,
                                 httplib::Response&      res) {
        if (!origin_allowed(req, host->opts_.allowed_origins)) {
            res.status = 403;
            res.set_content("Origin not allowed", "text/plain");
            return;
        }

        // Resolve or mint session id.
        std::string sid;
        if (auto it = req.headers.find("Mcp-Session-Id"); it != req.headers.end()) {
            sid = it->second;
        }
        const bool is_initialize_body = req.body.find("\"initialize\"") != std::string::npos;
        if (sid.empty() && is_initialize_body) {
            sid = make_session_id();
        }

        std::shared_ptr<SessionContext> ctx;
        if (!sid.empty()) {
            std::lock_guard<std::mutex> lk(host->impl_->sessions_mu);
            auto it = host->impl_->sessions.find(sid);
            if (it != host->impl_->sessions.end()) {
                ctx = it->second;
                ctx->last_seen = std::chrono::steady_clock::now();
            } else if (is_initialize_body) {
                ctx = std::make_shared<SessionContext>();
                ctx->id        = sid;
                ctx->last_seen = std::chrono::steady_clock::now();
                ctx->transport = std::make_shared<HttpSessionTransport>();
                ctx->server    = std::make_unique<Server>(host->server_info_);
                host->factory_(*ctx->server);

                // Run the Server on its own thread, wrapping our
                // shared_ptr transport in a unique_ptr-shaped shim so
                // Server::run can take ownership without disturbing
                // the host's own reference.
                auto* server_ptr = ctx->server.get();
                auto  transport  = ctx->transport;
                ctx->run_thread = std::thread([server_ptr, transport]() {
                    server_ptr->run(std::make_unique<SharedTransportShim>(transport));
                });
                host->impl_->sessions.emplace(sid, ctx);
            }
        }

        if (!ctx) {
            // Either no session id and not an initialize, or session
            // id refers to a session we don't know. Per spec, return
            // 400 to push the client to start a new session.
            res.status = 400;
            res.set_content("unknown session id; send initialize first",
                            "text/plain");
            return;
        }

        res.set_header("Mcp-Session-Id", ctx->id);

        // Hand the body to the per-session transport. If it was a
        // request, we'll get a future for the response; otherwise
        // we reply 202.
        auto reply = ctx->transport->deliver_request_or_notification(req.body);
        if (!reply.has_value()) {
            res.status = 202;
            return;
        }
        try {
            // 30s budget — matches the Session's default request
            // timeout. Beyond that the handler is misbehaving.
            const auto status = reply->wait_for(std::chrono::seconds{30});
            if (status != std::future_status::ready) {
                res.status = 504;
                res.set_content("handler timed out", "text/plain");
                return;
            }
            res.set_content(reply->get(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string{"handler error: "} + e.what(),
                            "text/plain");
        }
    });

    // GET: Streamable HTTP allows a long-lived SSE stream for
    // server-initiated traffic. Phase 3e/2 doesn't implement it; per
    // spec, returning 405 cleanly tells the client we don't offer
    // the stream.
    srv.Get(opts_.path, [host](const httplib::Request& req,
                                httplib::Response&     res) {
        if (!origin_allowed(req, host->opts_.allowed_origins)) {
            res.status = 403;
            res.set_content("Origin not allowed", "text/plain");
            return;
        }
        res.status = 405;
        res.set_content("server-initiated stream not offered on this build",
                        "text/plain");
    });

    // DELETE: terminate session.
    srv.Delete(opts_.path, [host](const httplib::Request& req,
                                    httplib::Response& res) {
        if (!origin_allowed(req, host->opts_.allowed_origins)) {
            res.status = 403;
            res.set_content("Origin not allowed", "text/plain");
            return;
        }
        auto it = req.headers.find("Mcp-Session-Id");
        if (it == req.headers.end()) {
            res.status = 400;
            return;
        }
        std::shared_ptr<SessionContext> ctx;
        {
            std::lock_guard<std::mutex> lk(host->impl_->sessions_mu);
            auto sit = host->impl_->sessions.find(it->second);
            if (sit != host->impl_->sessions.end()) {
                ctx = sit->second;
                host->impl_->sessions.erase(sit);
            }
        }
        if (ctx) {
            ctx->server->stop();
            if (ctx->run_thread.joinable()) ctx->run_thread.join();
            ctx->transport->close();
        }
        res.status = 204;
    });

    impl_->port = impl_->http.bind_to_any_port(opts_.host);
    if (impl_->port < 0) {
        started_.store(false, std::memory_order_release);
        throw std::runtime_error("HttpServerHost: bind failed");
    }
    if (opts_.port != 0 && opts_.port != impl_->port) {
        // bind_to_any_port ignores opts_.port; users who want a
        // specific port should use bind_to_port + listen instead.
        // For now, expose the actual bound port.
    }
    impl_->listener = std::thread([this] { impl_->http.listen_after_bind(); });
    while (!impl_->http.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds{1});
}

void HttpServerHost::stop() {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
    if (!started_.load(std::memory_order_acquire)) return;

    if (impl_) {
        impl_->http.stop();
        if (impl_->listener.joinable()) impl_->listener.join();

        std::unordered_map<std::string, std::shared_ptr<SessionContext>> drained;
        {
            std::lock_guard<std::mutex> lk(impl_->sessions_mu);
            drained.swap(impl_->sessions);
        }
        for (auto& [id, ctx] : drained) {
            ctx->server->stop();
            if (ctx->run_thread.joinable()) ctx->run_thread.join();
            ctx->transport->close();
        }
    }
}

}  // namespace mcp
