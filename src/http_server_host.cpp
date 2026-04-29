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
        // Just stash the callback. Crucially we do NOT yet allow
        // dispatch: Session::Session() wires on_message BEFORE
        // Server::run registers any of its request handlers. A frame
        // delivered in that window would route into an empty handler
        // map and emit a spurious "method not found: initialize"
        // response. The `started_` gate, flipped by start(), is what
        // marks the dispatcher as fully populated.
        std::lock_guard<std::mutex> lk(buffered_mu_);
        on_message_ = std::move(cb);
    }
    void on_error(ErrorCallback cb)     override { on_error_   = std::move(cb); }
    void on_close(CloseCallback cb)     override { on_close_   = std::move(cb); }

    void start() override {
        // Flip the gate and drain anything that arrived before all
        // request handlers were registered. By the time Session::start
        // runs us, set_request_handler in Server::run has completed.
        std::deque<std::string> drain;
        {
            std::lock_guard<std::mutex> lk(buffered_mu_);
            started_ = true;
            buffered_.swap(drain);
        }
        if (on_message_) {
            for (auto& f : drain) on_message_(std::move(f));
        }
    }

    void close() override {
        if (closed_.exchange(true, std::memory_order_acq_rel)) return;
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
        // Wake any GET stream waiter so the http handler can return.
        {
            std::lock_guard<std::mutex> lk(get_mu_);
            get_cv_.notify_all();
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

    [[nodiscard]] bool is_closed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::error_code send(std::string_view frame) override {
        if (closed_.load(std::memory_order_acquire)) {
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

        // Notification or server-initiated request. Always buffer
        // here: if a GET stream is already open, the GET handler
        // picks it up immediately; if not, frames wait until the
        // client opens its GET stream so a tool handler that fires
        // server.sample() right after initialize doesn't lose its
        // request to a startup race.
        std::lock_guard<std::mutex> lk(get_mu_);
        get_queue_.emplace_back(frame);
        get_cv_.notify_all();
        return {};
    }

    // Host-facing: block until a frame is available for the GET
    // stream, the close flag fires, or `until` is reached. Returns
    // a frame if one was available, std::nullopt otherwise. Caller
    // must call `enter_get_stream()` once before the first wait so
    // outbound traffic stops being dropped.
    void enter_get_stream() {
        std::lock_guard<std::mutex> lk(get_mu_);
        get_active_ = true;
    }
    void leave_get_stream() {
        std::lock_guard<std::mutex> lk(get_mu_);
        get_active_ = false;
        // Don't drain the queue: another GET might be opened (with
        // resumability). Phase 3e/3 ships without resumability so in
        // practice we simply lose the queue's contents on disconnect.
    }
    template <typename Rep, typename Period>
    std::optional<std::string>
    wait_get_frame(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lk(get_mu_);
        if (!get_cv_.wait_for(lk, timeout, [&] {
                return !get_queue_.empty()
                    || closed_.load(std::memory_order_acquire);
            })) return std::nullopt;
        if (get_queue_.empty()) return std::nullopt;
        std::string out = std::move(get_queue_.front());
        get_queue_.pop_front();
        return out;
    }

    [[nodiscard]] bool is_open() const noexcept override {
        // Open from construction until close(); see start() comment.
        return !closed_.load(std::memory_order_acquire);
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
        // Deliver under buffered_mu_ so the started_/buffered_/on_message_
        // triple stays consistent against start() and on_message().
        // started_ alone is the gate, not on_message_: the latter is
        // wired by Session::Session before set_request_handler calls
        // run, and dispatching during that window produces spurious
        // "method not found" errors.
        std::lock_guard<std::mutex> lk(buffered_mu_);
        if (started_ && on_message_) {
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

    std::atomic<bool>                                            closed_{false};

    // Pre-start buffer, the callback, and the started_ gate share a
    // mutex so their three-way consistency is maintained across the
    // setter (on_message), the gate flip (start), and the producer
    // (deliver_request_or_notification).
    std::mutex                                                   buffered_mu_;
    bool                                                         started_ = false;
    MessageCallback                                              on_message_;
    std::deque<std::string>                                      buffered_;

    ErrorCallback                                                on_error_;
    CloseCallback                                                on_close_;

    std::mutex                                                   pending_mu_;
    std::unordered_map<std::string, std::promise<std::string>>   pending_;

    // GET-stream queue (server-initiated outbound traffic). Frames
    // produced by `send()` while a GET stream is active are pushed
    // here and consumed by the GET handler.
    std::mutex                                                   get_mu_;
    std::condition_variable                                      get_cv_;
    std::deque<std::string>                                      get_queue_;
    bool                                                         get_active_ = false;
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

// Reject control chars + the two characters that need escaping in an
// RFC 7235 quoted-string. Using "reject" rather than "escape" because
// the only legitimate values for realm and metadata_url shouldn't
// contain any of these — and a malformed input is far more likely
// to be an attack than a legitimate exotic value.
inline bool valid_quoted_param(std::string_view s) noexcept {
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\' || c == '\r' || c == '\n' || c < 0x20) {
            return false;
        }
    }
    return true;
}

// Resolve the Access-Control-Allow-Origin value to echo back for a
// given request. If the host's allowed_origins list contains "*"
// the response advertises "*"; otherwise it echoes the request's
// Origin verbatim ONLY when the origin passes the same allowlist
// check that POST/GET/DELETE go through. Returns empty string if
// the origin would be rejected — caller skips the ACAO header.
inline std::string resolve_cors_origin(const httplib::Request&         req,
                                        const std::vector<std::string>& allow) {
    const auto it = req.headers.find("Origin");
    if (it == req.headers.end() || it->second.empty()) return {};
    for (const auto& a : allow) {
        if (a == "*") return "*";
        if (a == it->second) return it->second;
    }
    return {};
}

// Build the WWW-Authenticate Bearer challenge value for 401/403
// responses per RFC 6750 §3. error_code is "invalid_request" |
// "invalid_token" | "insufficient_scope" (or empty to omit). scope is
// the space-separated scope hint for insufficient_scope.
inline std::string build_bearer_challenge(const std::string&     realm,
                                           const std::string&     metadata_url,
                                           std::string_view       error_code,
                                           std::string_view       scope) {
    // valid_quoted_param() at start() guarantees realm + metadata_url
    // never contain unsafe characters; we just splice them.
    std::string out = "Bearer realm=\"";
    out += realm;
    out += "\"";
    if (!error_code.empty()) {
        out += ", error=\"";
        out += error_code;
        out += "\"";
    }
    if (!scope.empty()) {
        out += ", scope=\"";
        out += scope;
        out += "\"";
    }
    if (!metadata_url.empty()) {
        out += ", resource_metadata=\"";
        out += metadata_url;
        out += "\"";
    }
    return out;
}

// Case-insensitive ASCII prefix match. cpp-httplib's trim semantics
// don't normalize the scheme, so we have to. RFC 7235 §2.1 says
// scheme tokens are case-insensitive.
inline bool starts_with_bearer_ci(std::string_view s) noexcept {
    constexpr std::string_view prefix = "Bearer ";
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size() - 1; ++i) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
        if (a != b) return false;
    }
    // The separator after the scheme can be any OWS (space or tab).
    const char sep = s[prefix.size() - 1];
    return sep == ' ' || sep == '\t';
}

// Trim leading/trailing OWS from a token68 candidate.
inline std::string_view trim_ows(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
    return s;
}

// Per the 2025-11-25 spec: a request whose `MCP-Protocol-Version`
// header is present but unsupported MUST be rejected with 400. We
// know about three published revisions; the absence of the header
// is allowed (the spec lets clients omit it for the initial
// initialize call).
inline bool valid_protocol_version_header(std::string_view v) noexcept {
    return v == "2025-03-26" || v == "2025-06-18" || v == "2025-11-25";
}

// Validate the Authorization header against the host's bearer
// validator (if configured). Returns true when the request is allowed
// to proceed; if false, fills `res` with a 401/403 response.
inline bool authorize(const httplib::Request&         req,
                      httplib::Response&              res,
                      const HttpServerHost::Options&  opts) {
    if (!opts.bearer_validator) return true;

    const auto count_auth = req.headers.count("Authorization");
    if (count_auth == 0) {
        res.status = 401;
        res.set_header("WWW-Authenticate",
            build_bearer_challenge(opts.auth_realm,
                                   opts.resource_metadata_url,
                                   "invalid_request", {}));
        res.set_content("missing Authorization header", "text/plain");
        return false;
    }
    // RFC 7230: a request with multiple Authorization headers is
    // syntactically invalid — refuse rather than pick one arbitrarily.
    if (count_auth > 1) {
        res.status = 400;
        res.set_content("multiple Authorization headers", "text/plain");
        return false;
    }

    const auto it = req.headers.find("Authorization");
    const std::string& hdr = it->second;
    if (!starts_with_bearer_ci(hdr)) {
        res.status = 401;
        res.set_header("WWW-Authenticate",
            build_bearer_challenge(opts.auth_realm,
                                   opts.resource_metadata_url,
                                   "invalid_request", {}));
        res.set_content("expected Bearer scheme", "text/plain");
        return false;
    }
    constexpr std::size_t prefix_len = 7;  // "Bearer "
    auto token = trim_ows(std::string_view{hdr}.substr(prefix_len));
    if (token.empty()) {
        res.status = 401;
        res.set_header("WWW-Authenticate",
            build_bearer_challenge(opts.auth_realm,
                                   opts.resource_metadata_url,
                                   "invalid_request", {}));
        res.set_content("empty bearer token", "text/plain");
        return false;
    }

    const auto outcome = opts.bearer_validator(token);
    switch (outcome.status) {
        case HttpServerHost::Options::BearerStatus::allow:
            return true;
        case HttpServerHost::Options::BearerStatus::invalid_token:
            res.status = 401;
            res.set_header("WWW-Authenticate",
                build_bearer_challenge(opts.auth_realm,
                                       opts.resource_metadata_url,
                                       "invalid_token", {}));
            res.set_content("invalid bearer token", "text/plain");
            return false;
        case HttpServerHost::Options::BearerStatus::insufficient_scope:
            // Scope strings have the same control-char concern as the
            // realm. valid_quoted_param() is checked at start() but
            // validator output is fresh per request — drop unsafe
            // strings here rather than splicing them blindly.
            res.status = 403;
            res.set_header("WWW-Authenticate",
                build_bearer_challenge(opts.auth_realm,
                                       opts.resource_metadata_url,
                                       "insufficient_scope",
                                       valid_quoted_param(outcome.required_scopes)
                                           ? std::string_view{outcome.required_scopes}
                                           : std::string_view{}));
            res.set_content("insufficient scope", "text/plain");
            return false;
    }
    return false;  // unreachable
}

}  // namespace

// =====================================================================
// Lifecycle
// =====================================================================

void HttpServerHost::start() {
    if (started_.exchange(true, std::memory_order_acq_rel)) return;

    // Inputs that get spliced into HTTP response headers have to be
    // sanitised — a CR/LF or `"` would smuggle a header. We don't try
    // to escape; we reject. Legitimate realm names and URLs don't
    // contain these.
    if (!valid_quoted_param(opts_.auth_realm)) {
        started_.store(false, std::memory_order_release);
        throw std::invalid_argument(
            "HttpServerHost: auth_realm contains a control or quoting "
            "character — refusing to splice into the WWW-Authenticate "
            "header");
    }
    if (!valid_quoted_param(opts_.resource_metadata_url)) {
        started_.store(false, std::memory_order_release);
        throw std::invalid_argument(
            "HttpServerHost: resource_metadata_url contains a control "
            "or quoting character — refusing to splice into the "
            "WWW-Authenticate header");
    }
    // path is what we register routes under; allowing weird values
    // would conflict with the well-known endpoints below.
    if (opts_.path.empty() || opts_.path.front() != '/') {
        started_.store(false, std::memory_order_release);
        throw std::invalid_argument(
            "HttpServerHost: path must begin with '/'");
    }

    // Bearer auth over plain HTTP from a non-loopback address ships
    // tokens in the clear. The spec mandates HTTPS in production;
    // we let users opt into "I have a TLS terminator in front of
    // me" via allow_insecure_http rather than guess.
    const bool is_loopback = (opts_.host == "127.0.0.1" ||
                              opts_.host == "::1"       ||
                              opts_.host == "localhost");
    if (opts_.bearer_validator && !is_loopback &&
        !opts_.allow_insecure_http) {
        started_.store(false, std::memory_order_release);
        throw std::invalid_argument(
            "HttpServerHost: bearer authentication is configured but "
            "the host is binding to a non-loopback address (\"" +
            opts_.host + "\") and the SDK is on plain HTTP. Set "
            "Options::allow_insecure_http=true to acknowledge that "
            "TLS termination happens elsewhere (typically a reverse "
            "proxy), or bind to 127.0.0.1/::1 for development.");
    }

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
        // Spec MUST: a present-but-unsupported MCP-Protocol-Version
        // is a 400. Header is allowed to be absent.
        if (auto it = req.headers.find("MCP-Protocol-Version");
            it != req.headers.end() &&
            !valid_protocol_version_header(it->second)) {
            res.status = 400;
            res.set_content("unsupported MCP-Protocol-Version: " + it->second,
                            "text/plain");
            return;
        }
        if (!authorize(req, res, host->opts_)) return;

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
            const auto body = reply->get();
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string{"handler error: "} + e.what(),
                            "text/plain");
        }
    });

    // GET: open a long-lived SSE stream for server-initiated
    // traffic. The handler holds the connection open and emits one
    // SSE event per frame the per-session transport produces while
    // a request is pending or a notification fires.
    srv.Get(opts_.path, [host](const httplib::Request& req,
                                httplib::Response&     res) {
        if (!origin_allowed(req, host->opts_.allowed_origins)) {
            res.status = 403;
            res.set_content("Origin not allowed", "text/plain");
            return;
        }
        if (!authorize(req, res, host->opts_)) return;
        // Resolve the session by Mcp-Session-Id. Without one we have
        // no business holding open a stream — return 405 per spec
        // ("server does not offer an SSE stream").
        auto sid_it = req.headers.find("Mcp-Session-Id");
        if (sid_it == req.headers.end() || sid_it->second.empty()) {
            res.status = 405;
            return;
        }
        std::shared_ptr<SessionContext> ctx;
        {
            std::lock_guard<std::mutex> lk(host->impl_->sessions_mu);
            auto sit = host->impl_->sessions.find(sid_it->second);
            if (sit != host->impl_->sessions.end()) ctx = sit->second;
        }
        if (!ctx) {
            res.status = 404;
            return;
        }

        auto transport = ctx->transport;
        transport->enter_get_stream();

        res.set_chunked_content_provider(
            "text/event-stream",
            [transport](std::size_t /*offset*/,
                        httplib::DataSink& sink) -> bool {
                if (transport->is_closed() || !sink.is_writable()) {
                    sink.done();
                    return false;
                }
                auto frame = transport->wait_get_frame(std::chrono::seconds{15});
                if (!frame.has_value()) {
                    if (!sink.write(":\n\n", 3)) {
                        sink.done();
                        return false;
                    }
                    return true;
                }
                std::string sse = "data: ";
                sse.append(*frame);
                sse.append("\n\n");
                if (!sink.write(sse.data(), sse.size())) {
                    sink.done();
                    return false;
                }
                return true;
            },
            [transport](bool /*success*/) {
                transport->leave_get_stream();
            });
    });

    // DELETE: terminate session.
    srv.Delete(opts_.path, [host](const httplib::Request& req,
                                    httplib::Response& res) {
        if (!origin_allowed(req, host->opts_.allowed_origins)) {
            res.status = 403;
            res.set_content("Origin not allowed", "text/plain");
            return;
        }
        if (!authorize(req, res, host->opts_)) return;
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
            // Same TSan-race rationale as in stop(): close the
            // transport first, let its on_close cascade unblock the
            // server's run loop, then join.
            ctx->transport->close();
            if (ctx->run_thread.joinable()) ctx->run_thread.join();
        }
        res.status = 204;
    });

    // OPTIONS preflight on the MCP path so browser-based clients
    // can send Authorization, Mcp-Session-Id, MCP-Protocol-Version,
    // etc. cross-origin. The handler runs the same origin check as
    // the real verbs; if origin doesn't pass, we 403 — which the
    // browser surfaces to the JS client as a CORS failure (same as
    // any other denied preflight).
    if (opts_.enable_cors_preflight) {
        srv.Options(opts_.path, [host](const httplib::Request& req,
                                         httplib::Response&     res) {
            const auto acao = resolve_cors_origin(
                req, host->opts_.allowed_origins);
            if (acao.empty()) {
                res.status = 403;
                return;
            }
            res.set_header("Access-Control-Allow-Origin",  acao);
            res.set_header("Access-Control-Allow-Methods",
                           "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers",
                           "Authorization, Content-Type, Mcp-Session-Id, "
                           "MCP-Protocol-Version, Last-Event-ID, Accept");
            res.set_header("Access-Control-Expose-Headers",
                           "Mcp-Session-Id, WWW-Authenticate");
            res.set_header("Access-Control-Max-Age", "600");
            res.status = 204;
        });
    }

    // RFC 9728 Protected Resource Metadata. We expose the same
    // document at the canonical root well-known URI and at the
    // path-prefixed variant when path is non-trivial. NOT gated by
    // origin / Bearer — discovery is meant to be public, including
    // cross-origin (browser MCP clients run discovery from a foreign
    // origin), so we set the permissive CORS header per RFC 9728 §3.1.
    if (opts_.resource_metadata.has_value()) {
        const std::string body = opts_.resource_metadata->dump();
        const auto handler = [body](const httplib::Request&,
                                     httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Cache-Control", "public, max-age=3600");
            res.set_content(body, "application/json");
        };
        srv.Get("/.well-known/oauth-protected-resource", handler);
        // Only register the path-prefixed variant when path is more
        // than a bare "/"; otherwise the route would collide with
        // the canonical one above.
        if (opts_.path.size() > 1) {
            srv.Get("/.well-known/oauth-protected-resource" + opts_.path,
                    handler);
        }
    }

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
        // Close transports first so any in-flight handler (a GET
        // SSE stream parked in wait_get_frame, a POST awaiting a
        // sampling reply that will never arrive) can return. If we
        // joined the listener before this, those in-flight handlers
        // would block forever and the join would deadlock.
        //
        // We deliberately do NOT call ctx->server->stop() here even
        // though it would also unblock the run_thread's stop_cv:
        // both stop() and transport->close() race to wake T14, but
        // transport->close() also runs the Session's on_close hook
        // (which touches Session members). If stop() wakes T14 first
        // it will destroy the Session out from under that on_close
        // hook — TSan caught exactly that race in CI. Letting the
        // transport's on_close → user-on_closed cascade be the
        // sole wake-up path keeps "Session destruction" properly
        // happens-after "on_close finished".
        std::unordered_map<std::string, std::shared_ptr<SessionContext>> drained;
        {
            std::lock_guard<std::mutex> lk(impl_->sessions_mu);
            drained.swap(impl_->sessions);
        }
        for (auto& [id, ctx] : drained) {
            ctx->transport->close();
        }

        impl_->http.stop();
        if (impl_->listener.joinable()) impl_->listener.join();

        for (auto& [id, ctx] : drained) {
            if (ctx->run_thread.joinable()) ctx->run_thread.join();
        }
    }
}

}  // namespace mcp
