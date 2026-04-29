// SPDX-License-Identifier: Apache-2.0
//
// Streamable HTTP server host (multi-session).
//
// Whereas `Server::run(transport)` runs one MCP session over one
// transport, an HTTP server hosts many sessions concurrently — one
// per `Mcp-Session-Id`. `HttpServerHost` is the coordinator: it owns
// the underlying HTTP listener, mints session ids, wires each
// session to a fresh `mcp::Server` populated by a user-supplied
// session factory, and tears sessions down on DELETE / idle / stop.
//
// Usage:
//
//   mcp::HttpServerHost host{
//       mcp::Implementation{.name = "calc", .version = "1.0"},
//       mcp::HttpServerHost::Options{
//           .host = "127.0.0.1",
//           .port = 8080,
//           .path = "/mcp",
//           .allowed_origins = {"https://example.com"},
//       },
//       [](mcp::Server& s) {
//           s.tool("add", schema, handler);
//       },
//   };
//   host.start();
//   // ...later...
//   host.stop();

#pragma once

#if !defined(MCP_ENABLE_HTTP)
#error "mcp::HttpServerHost requires MCP_ENABLE_HTTP=1 (CMake option MCP_ENABLE_HTTP=ON)"
#endif

#include "mcp/protocol.hpp"
#include "mcp/server.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mcp {

class HttpServerHost {
public:
    /// Called for each new session. Register handlers on the supplied
    /// `Server`. The Server is freshly constructed for this session
    /// and owned by the host; do not capture references that outlive
    /// the call (tool handlers may, since they live on the Server).
    using SessionFactory = std::function<void(Server& server)>;

    struct Options {
        /// Bind address. Default to loopback so a misconfigured server
        /// is not accidentally exposed to the network.
        std::string host = "127.0.0.1";

        /// 0 = OS-assigned port; useful for tests. After start() the
        /// effective port is queryable via `port()`.
        int port = 0;

        /// Single endpoint path. POST + GET + DELETE on this path are
        /// the spec's Streamable HTTP surface; other paths get 404.
        std::string path = "/mcp";

        /// Origin allowlist. Empty means "reject every cross-origin
        /// request that sets an Origin header" — DNS-rebinding
        /// hardening. To accept all origins explicitly, set this to
        /// {"*"} (not recommended in production).
        std::vector<std::string> allowed_origins;

        /// Idle session timeout. After this many seconds with no
        /// inbound traffic, the session is torn down.
        std::chrono::seconds idle_timeout{600};
    };

    HttpServerHost(Implementation server_info,
                   Options        opts,
                   SessionFactory factory);
    ~HttpServerHost();

    HttpServerHost(const HttpServerHost&) = delete;
    HttpServerHost& operator=(const HttpServerHost&) = delete;
    HttpServerHost(HttpServerHost&&) = delete;
    HttpServerHost& operator=(HttpServerHost&&) = delete;

    /// Start the listener thread. Blocks until the listener is bound
    /// (so `port()` is meaningful immediately on return).
    void start();

    /// Stop the listener and tear down all sessions. Idempotent.
    void stop();

    /// The port the listener is actually bound to (resolves a 0 in
    /// Options::port to the OS-assigned value). Returns 0 before
    /// start().
    [[nodiscard]] int port() const noexcept;

    /// Number of sessions currently alive.
    [[nodiscard]] std::size_t active_sessions() const;

private:
    struct SessionContext;
    struct Impl;

    Implementation                                          server_info_;
    Options                                                 opts_;
    SessionFactory                                          factory_;

    std::atomic<bool>                                       started_{false};
    std::atomic<bool>                                       stopping_{false};
    std::unique_ptr<Impl>                                   impl_;
};

}  // namespace mcp
