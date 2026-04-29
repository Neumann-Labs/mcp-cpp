// SPDX-License-Identifier: Apache-2.0
#include "mcp/http_client_transport.hpp"

#include "mcp/log.hpp"

// cpp-httplib lives entirely in this TU; the public header is opaque.
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace mcp {

// =====================================================================
// pImpl
// =====================================================================

struct HttpClientTransport::ClientImpl {
    explicit ClientImpl(const std::string& base)
        : client_post(base), client_get(base) {}
    // cpp-httplib's Client is not safe to use concurrently on the
    // same instance. We need a long-lived GET in parallel with
    // many POSTs, so each direction has its own client.
    httplib::Client client_post;
    httplib::Client client_get;
};

namespace {

// Split a URL into the form cpp-httplib accepts (scheme://host[:port])
// and a path. Returns {scheme_host_port, path}. Path is empty for "/".
std::pair<std::string, std::string> split_url(std::string_view url) {
    // Find scheme delimiter
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        throw std::invalid_argument("HttpClientTransport: URL must contain a scheme (http:// or https://)");
    }
    const auto authority_start = scheme_end + 3;
    const auto path_start = url.find('/', authority_start);
    if (path_start == std::string_view::npos) {
        return {std::string{url}, std::string{"/"}};
    }
    return {std::string{url.substr(0, path_start)},
            std::string{url.substr(path_start)}};
}

// Iterate SSE frames in a buffer, calling `cb` with each event's `data`
// payload. `buf` is read up through the last complete event terminator
// (`\n\n` or `\r\n\r\n`); the unconsumed tail is left in `buf` for the
// next round. SSE supports multiple kinds of fields; we only care about
// `data:` lines for MCP. Multi-line `data:` is concatenated with `\n`
// per the SSE spec.
template <typename Callback>
void drain_sse(std::string& buf, Callback&& cb) {
    while (true) {
        std::size_t end = std::string::npos;
        std::size_t end_marker_len = 0;
        // Look for "\n\n" or "\r\n\r\n" — whichever comes first.
        for (std::size_t i = 0; i + 1 < buf.size(); ++i) {
            if (buf[i] == '\n' && buf[i + 1] == '\n') {
                end = i; end_marker_len = 2; break;
            }
            if (i + 3 < buf.size() && buf[i] == '\r' && buf[i + 1] == '\n'
                && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                end = i; end_marker_len = 4; break;
            }
        }
        if (end == std::string::npos) return;

        const std::string event{buf.data(), end};
        buf.erase(0, end + end_marker_len);

        // Each line in the event; gather data: payloads.
        std::string data;
        std::size_t line_start = 0;
        while (line_start <= event.size()) {
            const auto line_end = event.find('\n', line_start);
            std::string_view line;
            if (line_end == std::string::npos) {
                line = std::string_view{event}.substr(line_start);
                line_start = event.size() + 1;
            } else {
                line = std::string_view{event}.substr(line_start, line_end - line_start);
                line_start = line_end + 1;
            }
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || line[0] == ':') continue;  // comment/keepalive
            const auto colon = line.find(':');
            std::string_view field, value;
            if (colon == std::string_view::npos) {
                field = line; value = {};
            } else {
                field = line.substr(0, colon);
                value = line.substr(colon + 1);
                if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            }
            if (field == "data") {
                if (!data.empty()) data += '\n';
                data.append(value);
            }
            // Other fields (`event:`, `id:`, `retry:`) are ignored for now.
        }
        if (!data.empty()) cb(std::move(data));
    }
}

}  // namespace

// =====================================================================
// Construction / destruction
// =====================================================================

HttpClientTransport::HttpClientTransport(Options opts) : opts_(std::move(opts)) {
    auto split = split_url(opts_.url);
    scheme_host_port_ = std::move(split.first);
    path_             = std::move(split.second);
    if (opts_.session_id.has_value()) {
        session_id_ = *opts_.session_id;
    }
    impl_ = std::make_unique<ClientImpl>(scheme_host_port_);
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        opts_.connect_timeout).count();
    const auto usecs = (opts_.connect_timeout.count() % 1000) * 1000;
    const auto rt_secs = std::chrono::duration_cast<std::chrono::seconds>(
        opts_.request_timeout).count();
    const auto rt_usecs = (opts_.request_timeout.count() % 1000) * 1000;

    impl_->client_post.set_connection_timeout(secs, usecs);
    impl_->client_post.set_read_timeout(rt_secs, rt_usecs);
    impl_->client_post.set_write_timeout(rt_secs, rt_usecs);
    impl_->client_get.set_connection_timeout(secs, usecs);
    // GET stream stays open indefinitely; let it sit on the read
    // path without timing out. Heartbeats keep it alive.
    impl_->client_get.set_read_timeout(60 * 60, 0);
    impl_->client_get.set_write_timeout(rt_secs, rt_usecs);
}

HttpClientTransport::~HttpClientTransport() { close(); }

void HttpClientTransport::on_message(MessageCallback cb) { on_message_ = std::move(cb); }
void HttpClientTransport::on_error(ErrorCallback cb)     { on_error_   = std::move(cb); }
void HttpClientTransport::on_close(CloseCallback cb)     { on_close_   = std::move(cb); }

std::optional<std::string> HttpClientTransport::session_id() const {
    std::lock_guard<std::mutex> lk(session_id_mu_);
    return session_id_;
}

// =====================================================================
// Lifecycle
// =====================================================================

void HttpClientTransport::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        return;
    }
    closed_.store(false, std::memory_order_release);
    close_fired_.store(false, std::memory_order_release);
    worker_ = std::thread([this] { worker_loop(); });
    if (opts_.open_get_stream) {
        get_thread_ = std::thread([this] { run_get_stream(); });
    }
}

void HttpClientTransport::close() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        if (worker_.joinable()) worker_.join();
        if (get_thread_.joinable()) get_thread_.join();
        return;
    }

    {
        std::lock_guard<std::mutex> lk(out_mu_);
        out_cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(session_id_mu_);
        session_id_cv_.notify_all();
    }
    if (impl_) {
        impl_->client_post.stop();
        impl_->client_get.stop();
    }

    if (worker_.joinable()) worker_.join();
    if (get_thread_.joinable()) get_thread_.join();

    {
        std::unique_lock<std::mutex> lk(inflight_mu_);
        inflight_cv_.wait(lk, [this] { return inflight_ == 0; });
    }
    fire_close();
}

void HttpClientTransport::fire_close() noexcept {
    if (close_fired_.exchange(true, std::memory_order_acq_rel)) return;
    if (!on_close_) return;
    try { on_close_(); }
    catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"HttpClientTransport on_close threw: "} + e.what());
    } catch (...) {
        MCP_LOG_ERROR("HttpClientTransport on_close threw a non-std exception");
    }
}

void HttpClientTransport::deliver_error(std::error_code ec) noexcept {
    if (!on_error_) return;
    try { on_error_(ec); }
    catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"HttpClientTransport on_error threw: "} + e.what());
    } catch (...) {
        MCP_LOG_ERROR("HttpClientTransport on_error threw a non-std exception");
    }
}

void HttpClientTransport::deliver_frame(std::string raw) {
    if (!on_message_) return;
    try { on_message_(std::move(raw)); }
    catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"HttpClientTransport on_message threw: "} + e.what());
    } catch (...) {
        MCP_LOG_ERROR("HttpClientTransport on_message threw a non-std exception");
    }
}

// =====================================================================
// Send path
// =====================================================================

std::error_code HttpClientTransport::send(std::string_view frame) {
    if (!started_.load(std::memory_order_acquire) ||
         closed_.load(std::memory_order_acquire)) {
        return std::make_error_code(std::errc::not_connected);
    }
    {
        std::lock_guard<std::mutex> lk(out_mu_);
        out_.emplace_back(frame);
        out_cv_.notify_one();
    }
    return {};
}

void HttpClientTransport::worker_loop() {
    // Each outbound frame is processed on its own thread so a slow
    // POST (e.g. one held open by the server while it's awaiting a
    // sampling reply that needs to come back through us) doesn't
    // serialise with later sends. The host's POST handler can hold
    // a request open for tens of seconds; if the worker were
    // single-threaded, replying from inside that window would
    // deadlock.
    while (true) {
        std::string frame;
        {
            std::unique_lock<std::mutex> lk(out_mu_);
            out_cv_.wait(lk, [this] {
                return closed_.load(std::memory_order_acquire) || !out_.empty();
            });
            if (closed_.load(std::memory_order_acquire) && out_.empty()) return;
            frame = std::move(out_.front());
            out_.pop_front();
        }
        {
            std::lock_guard<std::mutex> lk(inflight_mu_);
            ++inflight_;
        }
        std::thread([this, f = std::move(frame)]() mutable {
            process_send(std::move(f));
            std::lock_guard<std::mutex> lk(inflight_mu_);
            --inflight_;
            inflight_cv_.notify_all();
        }).detach();
    }
}

void HttpClientTransport::process_send(std::string frame) {
    httplib::Headers headers;
    headers.emplace("Accept", "application/json, text/event-stream");
    headers.emplace("MCP-Protocol-Version", opts_.protocol_version);
    {
        std::lock_guard<std::mutex> lk(session_id_mu_);
        if (session_id_.has_value()) headers.emplace("Mcp-Session-Id", *session_id_);
    }
    if (opts_.access_token.has_value() && !opts_.access_token->empty()) {
        headers.emplace("Authorization", "Bearer " + *opts_.access_token);
    }
    for (const auto& [k, v] : opts_.extra_headers) headers.emplace(k, v);

    // Each send runs on its own thread (worker_loop dispatches), so
    // we use a per-call Client instance — cpp-httplib's Client is not
    // safe under concurrent use of the same instance, and using one
    // shared instance under a mutex would re-introduce the
    // serialisation deadlock we were just fighting.
    httplib::Client cli{scheme_host_port_};
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        opts_.connect_timeout).count();
    const auto usecs = (opts_.connect_timeout.count() % 1000) * 1000;
    const auto rt_secs = std::chrono::duration_cast<std::chrono::seconds>(
        opts_.request_timeout).count();
    const auto rt_usecs = (opts_.request_timeout.count() % 1000) * 1000;
    cli.set_connection_timeout(secs, usecs);
    cli.set_read_timeout(rt_secs, rt_usecs);
    cli.set_write_timeout(rt_secs, rt_usecs);
    auto res = cli.Post(path_.c_str(), headers, frame, "application/json");
    if (!res) {
        deliver_error(std::make_error_code(std::errc::io_error));
        return;
    }

    // Capture session id if the server set one. Notify the GET
    // worker so it can issue its first GET right away, instead of
    // sleeping out a poll interval.
    if (auto it = res->headers.find("Mcp-Session-Id");
        it != res->headers.end() && !it->second.empty()) {
        bool first_set = false;
        {
            std::lock_guard<std::mutex> lk(session_id_mu_);
            first_set = !session_id_.has_value();
            session_id_ = it->second;
        }
        if (first_set) session_id_cv_.notify_all();
    }

    if (res->status == 202) {
        // Empty 202 Accepted — server absorbed a notification or response.
        return;
    }
    if (res->status == 401 || res->status == 403) {
        // Surface OAuth challenge to the application via the
        // optional callback. The transport itself can't drive the
        // OAuth dance; the application either updates `access_token`
        // and retries, or destroys this transport and creates a new
        // one with the new token.
        std::string challenge;
        if (auto it = res->headers.find("WWW-Authenticate");
            it != res->headers.end()) {
            challenge = it->second;
        }
        if (opts_.on_unauthorized) {
            try { opts_.on_unauthorized(res->status, challenge); }
            catch (...) {}
        }
        deliver_error(std::make_error_code(std::errc::permission_denied));
        return;
    }
    if (res->status < 200 || res->status >= 300) {
        deliver_error(std::make_error_code(std::errc::protocol_error));
        return;
    }

    auto ct_it = res->headers.find("Content-Type");
    const std::string ct = (ct_it != res->headers.end()) ? ct_it->second : "";

    if (ct.find("text/event-stream") != std::string::npos) {
        std::string buf = res->body;
        drain_sse(buf, [this](std::string data) { deliver_frame(std::move(data)); });
        return;
    }
    if (ct.find("application/json") != std::string::npos) {
        deliver_frame(std::move(res->body));
        return;
    }
    // Unknown content-type — try to pass body anyway.
    deliver_frame(std::move(res->body));
}

// =====================================================================
// GET stream (server-initiated)
// =====================================================================

void HttpClientTransport::run_get_stream() noexcept {
    // The server's GET handler needs Mcp-Session-Id; it has no way
    // to associate an anonymous GET with the right session. So this
    // worker waits on a cv for the session id to be negotiated by
    // a POST response, and issues the GET as soon as it arrives —
    // no polling latency between "initialize() returned" and "GET
    // stream is open" (which a tool handler triggering server.sample()
    // would otherwise race).
    while (started_.load(std::memory_order_acquire) &&
          !closed_.load(std::memory_order_acquire)) {
        std::optional<std::string> sid;
        {
            std::unique_lock<std::mutex> lk(session_id_mu_);
            session_id_cv_.wait(lk, [this] {
                return session_id_.has_value() ||
                       closed_.load(std::memory_order_acquire);
            });
            if (closed_.load(std::memory_order_acquire)) return;
            sid = session_id_;
        }
        if (!sid.has_value()) continue;
        std::string buf;
        httplib::Headers headers;
        headers.emplace("Accept", "text/event-stream");
        headers.emplace("MCP-Protocol-Version", opts_.protocol_version);
        headers.emplace("Mcp-Session-Id", *sid);
        if (opts_.access_token.has_value() && !opts_.access_token->empty()) {
            headers.emplace("Authorization", "Bearer " + *opts_.access_token);
        }
        for (const auto& [k, v] : opts_.extra_headers) headers.emplace(k, v);

        httplib::Result res;
        try {
            res = impl_->client_get.Get(path_.c_str(), headers,
                [&](const char* data, std::size_t n) {
                    if (closed_.load(std::memory_order_acquire)) return false;
                    buf.append(data, n);
                    drain_sse(buf,
                        [this](std::string d) { deliver_frame(std::move(d)); });
                    return true;
                });
        } catch (...) {
            res = httplib::Result{};
        }

        // 405 Method Not Allowed is the spec's way of saying "no
        // server-initiated stream"; retry isn't useful, exit cleanly.
        if (res && res->status == 405) return;

        if (closed_.load(std::memory_order_acquire)) return;

        // Brief backoff before reconnecting after EOF/error.
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
}

}  // namespace mcp
