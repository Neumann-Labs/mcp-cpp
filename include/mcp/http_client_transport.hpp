// SPDX-License-Identifier: Apache-2.0
//
// Streamable HTTP client transport.
//
// Implements the client side of the MCP "Streamable HTTP" transport
// (spec 2025-11-25). One transport instance corresponds to one logical
// MCP session against a single HTTP endpoint:
//
//   * `send()` issues an HTTP POST to the endpoint with the JSON-RPC
//     frame as the body. The response Content-Type determines the
//     fan-out: `application/json` is delivered as a single frame to
//     the on_message callback; `text/event-stream` is parsed as SSE
//     and each `data:` event payload is delivered as its own frame.
//     A 202 Accepted response (sent for inbound notifications and
//     responses) is silently absorbed.
//   * On `start()`, an optional GET stream is opened to the same
//     endpoint to receive server-initiated requests / notifications
//     (sampling, log messages, list-changed, etc.). The stream is
//     parsed as SSE.
//   * The optional `Mcp-Session-Id` header is round-tripped: any
//     server response that sets it is captured, and every subsequent
//     request includes it. The negotiated id is exposed via
//     `session_id()`.
//
// Threading: an internal worker thread pumps both POST replies and
// the GET stream. Calls to `send()` are dispatched onto the worker so
// the caller does not block on the HTTP round-trip.

#pragma once

#if !defined(MCP_ENABLE_HTTP)
#error "mcp::HttpClientTransport requires MCP_ENABLE_HTTP=1 (CMake option MCP_ENABLE_HTTP=ON)"
#endif

#include "mcp/transport.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace mcp {

class HttpClientTransport final : public Transport {
public:
    struct Options {
        /// Endpoint URL, e.g. "http://localhost:8080/mcp".
        std::string url;

        /// MCP protocol version sent on every request via the
        /// `MCP-Protocol-Version` header. Defaults to the latest spec
        /// version this SDK supports.
        std::string protocol_version = "2025-11-25";

        /// Optional pre-existing session id (resume case). Leave empty
        /// for a fresh session — the server will assign one on the
        /// initialize response, and we'll capture it.
        std::optional<std::string> session_id;

        /// Connect timeout (TCP handshake + TLS).
        std::chrono::milliseconds connect_timeout{10'000};

        /// Per-request total timeout.
        std::chrono::milliseconds request_timeout{30'000};

        /// Extra HTTP headers attached to every outbound request.
        std::unordered_map<std::string, std::string> extra_headers;

        /// If true, open a long-lived GET stream on `start()` so the
        /// server can push requests and notifications. If false, the
        /// transport is request/response-only (no server-initiated
        /// traffic).
        bool open_get_stream = true;

        // -------- 2025-11-25 OAuth 2.1 authorization (optional) --------
        //
        // When `access_token` is non-empty, every outbound POST and
        // GET carries `Authorization: Bearer <access_token>` per
        // RFC 6750. The SDK does NOT itself perform the OAuth flow
        // (PKCE, dynamic client registration, code exchange) — the
        // application is expected to obtain a token (via Protected
        // Resource Metadata discovery + AS interaction) and supply
        // it here.
        std::optional<std::string> access_token;

        /// Hook fired when the server replies 401/403 to a request.
        /// Receives the raw `WWW-Authenticate` header value (may be
        /// empty if the server omits it). The application can parse
        /// it for `resource_metadata="<url>"` to drive discovery.
        /// After this fires, the offending POST has already failed
        /// and the transport delivers an error to the caller; the
        /// callback is purely a side-channel notification.
        std::function<void(int                status,
                           std::string_view   www_authenticate)>
            on_unauthorized;
    };

    explicit HttpClientTransport(Options opts);
    ~HttpClientTransport() override;

    // Transport interface ---------------------------------------------------

    void on_message(MessageCallback cb) override;
    void on_error(ErrorCallback cb)     override;
    void on_close(CloseCallback cb)     override;

    void start() override;
    void close() override;

    [[nodiscard]] std::error_code send(std::string_view frame) override;
    [[nodiscard]] bool is_open() const noexcept override {
        return started_.load(std::memory_order_acquire) &&
               !closed_.load(std::memory_order_acquire);
    }

    // HTTP-specific ---------------------------------------------------------

    /// The negotiated `Mcp-Session-Id`, or empty if none has been
    /// established yet (the server may opt out).
    [[nodiscard]] std::optional<std::string> session_id() const;

private:
    void worker_loop();
    void process_send(std::string frame);
    void deliver_frame(std::string raw);
    void deliver_error(std::error_code ec) noexcept;
    void fire_close() noexcept;
    void run_get_stream() noexcept;

    Options opts_;

    // Parsed url components — split once at construction so each send
    // doesn't re-parse.
    std::string scheme_host_port_;  // e.g. "http://localhost:8080"
    std::string path_;              // e.g. "/mcp"

    std::atomic<bool> started_{false};
    std::atomic<bool> closed_{false};
    std::atomic<bool> close_fired_{false};

    // Outbound queue: frames the application has handed us that are
    // waiting to go out as POSTs. The worker thread drains, then
    // dispatches each frame to its own short-lived sender thread so
    // many POSTs can be in flight simultaneously without serialising
    // (a long-blocked POST awaiting a server-initiated sampling
    // round-trip would otherwise wedge later sends).
    std::mutex                    out_mu_;
    std::condition_variable       out_cv_;
    std::deque<std::string>       out_;
    std::thread                   worker_;

    // Track in-flight per-send threads so close() can drain.
    std::mutex                    inflight_mu_;
    std::condition_variable       inflight_cv_;
    int                           inflight_ = 0;

    // GET-stream worker (optional).
    std::thread                   get_thread_;

    // Negotiated session id, captured from any inbound
    // `Mcp-Session-Id` response header. The GET worker waits on this
    // cv until a session id has been negotiated (or close fires).
    mutable std::mutex            session_id_mu_;
    std::condition_variable       session_id_cv_;
    std::optional<std::string>    session_id_;

    MessageCallback               on_message_;
    ErrorCallback                 on_error_;
    CloseCallback                 on_close_;

    // pImpl-style holder for the cpp-httplib Client, kept opaque from
    // the public header so consumers don't need cpp-httplib's
    // include path on their own translation units.
    struct ClientImpl;
    std::unique_ptr<ClientImpl>   impl_;
};

}  // namespace mcp
